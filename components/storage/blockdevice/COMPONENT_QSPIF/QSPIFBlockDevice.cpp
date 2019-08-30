/* mbed Microcontroller Library
 * Copyright (c) 2018 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "QSPIFBlockDevice.h"
#include <string.h>
#include "rtos/ThisThread.h"

#ifndef MBED_CONF_MBED_TRACE_ENABLE
#define MBED_CONF_MBED_TRACE_ENABLE        0
#endif

#include "mbed_trace.h"
#define TRACE_GROUP "QSPIF"

using namespace mbed;

/* Default QSPIF Parameters */
/****************************/
#define QSPIF_DEFAULT_PAGE_SIZE  256
#define QSPIF_DEFAULT_SE_SIZE    4096
#define QSPI_STATUS_REGISTER_COUNT 2
#ifndef UINT64_MAX
#define UINT64_MAX -1
#endif
#define QSPI_NO_ADDRESS_COMMAND UINT64_MAX
// Status Register Bits
#define QSPIF_STATUS_BIT_WIP        0x1 //Write In Progress
#define QSPIF_STATUS_BIT_WEL        0x2 // Write Enable Latch

/* SFDP Header Parsing */
/***********************/
#define QSPIF_RSFDP_DUMMY_CYCLES 8
#define QSPIF_SFDP_HEADER_SIZE 8
#define QSPIF_PARAM_HEADER_SIZE 8

/* Basic Parameters Table Parsing */
/**********************************/
#define SFDP_DEFAULT_BASIC_PARAMS_TABLE_SIZE_BYTES 64 /* 16 DWORDS */
//READ Instruction support according to BUS Configuration
#define QSPIF_BASIC_PARAM_TABLE_FAST_READ_SUPPORT_BYTE 2
#define QSPIF_BASIC_PARAM_TABLE_QPI_READ_SUPPORT_BYTE 16
#define QSPIF_BASIC_PARAM_TABLE_444_READ_INST_BYTE 27
#define QSPIF_BASIC_PARAM_TABLE_144_READ_INST_BYTE 9
#define QSPIF_BASIC_PARAM_TABLE_114_READ_INST_BYTE 11
#define QSPIF_BASIC_PARAM_TABLE_222_READ_INST_BYTE 23
#define QSPIF_BASIC_PARAM_TABLE_122_READ_INST_BYTE 15
#define QSPIF_BASIC_PARAM_TABLE_112_READ_INST_BYTE 13
#define QSPIF_BASIC_PARAM_TABLE_PAGE_SIZE_BYTE 40
// Quad Enable Params
#define QSPIF_BASIC_PARAM_TABLE_QER_BYTE 58
#define QSPIF_BASIC_PARAM_TABLE_444_MODE_EN_SEQ_BYTE 56
// Erase Types Params
#define QSPIF_BASIC_PARAM_ERASE_TYPE_1_BYTE 29
#define QSPIF_BASIC_PARAM_ERASE_TYPE_2_BYTE 31
#define QSPIF_BASIC_PARAM_ERASE_TYPE_3_BYTE 33
#define QSPIF_BASIC_PARAM_ERASE_TYPE_4_BYTE 35
#define QSPIF_BASIC_PARAM_ERASE_TYPE_1_SIZE_BYTE 28
#define QSPIF_BASIC_PARAM_ERASE_TYPE_2_SIZE_BYTE 30
#define QSPIF_BASIC_PARAM_ERASE_TYPE_3_SIZE_BYTE 32
#define QSPIF_BASIC_PARAM_ERASE_TYPE_4_SIZE_BYTE 34
#define QSPIF_BASIC_PARAM_4K_ERASE_TYPE_BYTE 1

#define QSPIF_BASIC_PARAM_TABLE_SOFT_RESET_BYTE 61
#define QSPIF_BASIC_PARAM_TABLE_4BYTE_ADDR_BYTE 63

#define SOFT_RESET_RESET_INST_BITMASK            0b001000
#define SOFT_RESET_ENABLE_AND_RESET_INST_BITMASK 0b010000

// Erase Types Per Region BitMask
#define ERASE_BITMASK_TYPE4 0x08
#define ERASE_BITMASK_TYPE1 0x01
#define ERASE_BITMASK_NONE  0x00
#define ERASE_BITMASK_ALL   0x0F

// 4-Byte Addressing Support Bitmasks
#define FOURBYTE_ADDR_B7_BITMASK           0b00000001
#define FOURBYTE_ADDR_B7_WREN_BITMASK      0b00000010
#define FOURBYTE_ADDR_EXT_ADDR_REG_BITMASK 0b00000100
#define FOURBYTE_ADDR_BANK_REG_BITMASK     0b00001000
#define FOURBYTE_ADDR_CONF_REG_BITMASK     0b00010000
#define FOURBYTE_ADDR_INSTS_BITMASK        0b00100000
#define FOURBYTE_ADDR_ALWAYS_BITMASK       0b01000000

#define IS_MEM_READY_MAX_RETRIES 10000

enum qspif_default_instructions {
    QSPIF_NOP  = 0x00, // No operation
    QSPIF_PP = 0x02, // Page Program data
    QSPIF_READ = 0x03, // Read data
    QSPIF_SE   = 0x20, // 4KB Sector Erase
    QSPIF_SFDP = 0x5a, // Read SFDP
    QSPIF_WRSR = 0x01, // Write Status/Configuration Register
    QSPIF_WRDI = 0x04, // Write Disable
    QSPIF_RDSR = 0x05, // Read Status Register
    QSPIF_WREN = 0x06, // Write Enable
    QSPIF_RSTEN = 0x66, // Reset Enable
    QSPIF_RST = 0x99, // Reset
    QSPIF_RDID = 0x9f, // Read Manufacturer and JDEC Device ID
    QSPIF_ULBPR = 0x98, // Clears all write-protection bits in the Block-Protection register
};

// Local Function
static int local_math_power(int base, int exp);

// General QSPI instructions
#define QSPIF_INST_WSR1  0x01 // Write status register 1
#define QSPIF_INST_RSR1  0x05 // Read status register 1
#define QSPIF_INST_RSFDP 0x5A // Read SFDP
#define QSPIF_INST_RDID  0x9F // Read Manufacturer and JDEC Device ID

// Device-specific instructions
#define QSPIF_INST_ULBPR 0x98 // Clear all write-protection bits in the Block-Protection register

// Default status register 2 read/write instructions
#define QSPIF_INST_WSR2_DEFAULT    QSPI_NO_INST
#define QSPIF_INST_RSR2_DEFAULT    0x35

// Default 4-byte extended addressing register write instruction
#define QSPIF_INST_4BYTE_REG_WRITE_DEFAULT QSPI_NO_INST


// Length of data returned from RDID instruction
#define QSPI_RDID_DATA_LENGTH 3


/* Init function to initialize Different Devices CS static list */
static PinName *generate_initialized_active_qspif_csel_arr();
// Static Members for different devices csel
// _devices_mutex is used to lock csel list - only one QSPIFBlockDevice instance per csel is allowed
SingletonPtr<PlatformMutex> QSPIFBlockDevice::_devices_mutex;
int QSPIFBlockDevice::_number_of_active_qspif_flash_csel = 0;
PinName *QSPIFBlockDevice::_active_qspif_flash_csel_arr = generate_initialized_active_qspif_csel_arr();

/********* Public API Functions *********/
/****************************************/
QSPIFBlockDevice::QSPIFBlockDevice(PinName io0, PinName io1, PinName io2, PinName io3, PinName sclk, PinName csel,
                                   int clock_mode, int freq)
    : _qspi(io0, io1, io2, io3, sclk, csel, clock_mode), _csel(csel), _freq(freq), _device_size_bytes(0),
      _init_ref_count(0),
      _is_initialized(false)
{
    _unique_device_status = add_new_csel_instance(csel);

    if (_unique_device_status == 0) {
        tr_debug("Adding a new QSPIFBlockDevice csel: %d\n", (int)csel);
    } else if (_unique_device_status == -1) {
        tr_error("QSPIFBlockDevice with the same csel(%d) already exists\n", (int)csel);
    } else {
        tr_error("Too many different QSPIFBlockDevice devices - max allowed: %d\n", QSPIF_MAX_ACTIVE_FLASH_DEVICES);
    }

    // Set default status register 2 write/read instructions
    _write_status_reg_2_inst = QSPIF_INST_WSR2_DEFAULT;
    _read_status_reg_2_inst = QSPIF_INST_RSR2_DEFAULT;

    // Set default 4-byte addressing extension register write instruction
    _4byte_msb_reg_write_inst = QSPIF_INST_4BYTE_REG_WRITE_DEFAULT;
}

int QSPIFBlockDevice::init()
{
    if (_unique_device_status == 0) {
        tr_debug("QSPIFBlockDevice csel: %d", (int)_csel);
    } else if (_unique_device_status == -1) {
        tr_error("QSPIFBlockDevice with the same csel(%d) already exists", (int)_csel);
        return QSPIF_BD_ERROR_DEVICE_NOT_UNIQE;
    } else {
        tr_error("Too many different QSPIFBlockDevice devices - max allowed: %d", QSPIF_MAX_ACTIVE_FLASH_DEVICES);
        return QSPIF_BD_ERROR_DEVICE_MAX_EXCEED;
    }

    int status = QSPIF_BD_ERROR_OK;
    uint32_t basic_table_addr = 0;
    size_t basic_table_size = 0;
    uint32_t sector_map_table_addr = 0;
    size_t sector_map_table_size = 0;

    _mutex.lock();

    // All commands other than Read and RSFDP use default 1-1-1 bus mode (Program/Erase are constrained by flash memory performance more than bus performance)
    _qspi.configure_format(QSPI_CFG_BUS_SINGLE, QSPI_CFG_BUS_SINGLE, _address_size, QSPI_CFG_BUS_SINGLE,
                           QSPI_CFG_ALT_SIZE_8, QSPI_CFG_BUS_SINGLE, 0);

    if (!_is_initialized) {
        _init_ref_count = 0;
    }

    _init_ref_count++;

    if (_init_ref_count != 1) {
        goto exit_point;
    }

    //Initialize parameters
    _min_common_erase_size = 0;
    _regions_count = 1;
    _region_erase_types_bitfield[0] = ERASE_BITMASK_NONE;

    //Default Bus Setup 1_1_1 with 0 dummy and mode cycles
    _inst_width = QSPI_CFG_BUS_SINGLE;
    _address_width = QSPI_CFG_BUS_SINGLE;
    _address_size = QSPI_CFG_ADDR_SIZE_24;
    _data_width = QSPI_CFG_BUS_SINGLE;
    _dummy_and_mode_cycles = 0;

    if (QSPI_STATUS_OK != _qspi_set_frequency(_freq)) {
        tr_error("QSPI Set Frequency Failed");
        status = QSPIF_BD_ERROR_DEVICE_ERROR;
        goto exit_point;
    }

    //Synchronize Device
    if (false == _is_mem_ready()) {
        tr_error("Init - _is_mem_ready Failed");
        status = QSPIF_BD_ERROR_READY_FAILED;
        goto exit_point;
    }

    /**************************** Parse SFDP Header ***********************************/
    if (0 != _sfdp_parse_sfdp_headers(basic_table_addr, basic_table_size, sector_map_table_addr, sector_map_table_size)) {
        tr_error("Init - Parse SFDP Headers Failed");
        status = QSPIF_BD_ERROR_PARSING_FAILED;
        goto exit_point;
    }

    /**************************** Parse Basic Parameters Table ***********************************/
    if (0 != _sfdp_parse_basic_param_table(basic_table_addr, basic_table_size)) {
        tr_error("Init - Parse Basic Param Table Failed");
        status = QSPIF_BD_ERROR_PARSING_FAILED;
        goto exit_point;
    }

    /**************************** Parse Sector Map Table ***********************************/
    _region_size_bytes[0] =
        _device_size_bytes; // If there's no region map, we have a single region sized the entire device size
    _region_high_boundary[0] = _device_size_bytes - 1;

    if ((sector_map_table_addr != 0) && (0 != sector_map_table_size)) {
        tr_debug("Init - Parsing Sector Map Table - addr: 0x%lxh, Size: %d", sector_map_table_addr,
                 sector_map_table_size);
        if (0 != _sfdp_parse_sector_map_table(sector_map_table_addr, sector_map_table_size)) {
            tr_error("Init - Parse Sector Map Table Failed");
            status = QSPIF_BD_ERROR_PARSING_FAILED;
            goto exit_point;
        }
    }

    if (0 != _clear_block_protection()) {
        tr_error("Init - clearing block protection failed");
        status = QSPIF_BD_ERROR_PARSING_FAILED;
        goto exit_point;
    }

    _is_initialized = true;

exit_point:
    _mutex.unlock();

    return status;
}

int QSPIFBlockDevice::deinit()
{
    int result = QSPIF_BD_ERROR_OK;

    _mutex.lock();

    if (!_is_initialized) {
        _init_ref_count = 0;
        _mutex.unlock();
        return result;
    }

    _init_ref_count--;

    if (_init_ref_count) {
        _mutex.unlock();
        return result;
    }

    // Disable Device for Writing
    qspi_status_t status = _qspi_send_general_command(QSPIF_WRDI, QSPI_NO_ADDRESS_COMMAND, NULL, 0, NULL, 0);
    if (status != QSPI_STATUS_OK)  {
        tr_error("Write Disable failed");
        result = QSPIF_BD_ERROR_DEVICE_ERROR;
    }

    _is_initialized = false;

    _mutex.unlock();

    if (_unique_device_status == 0) {
        remove_csel_instance(_csel);
    }

    return result;
}

int QSPIFBlockDevice::read(void *buffer, bd_addr_t addr, bd_size_t size)
{
    int status = QSPIF_BD_ERROR_OK;
    tr_debug("Read Inst: 0x%xh", _read_instruction);

    _mutex.lock();

    if (QSPI_STATUS_OK != _qspi_send_read_command(_read_instruction, buffer, addr, size)) {
        status = QSPIF_BD_ERROR_DEVICE_ERROR;
        tr_error("Read Command failed");
    }

    _mutex.unlock();
    return status;

}

int QSPIFBlockDevice::program(const void *buffer, bd_addr_t addr, bd_size_t size)
{
    qspi_status_t result = QSPI_STATUS_OK;
    bool program_failed = false;
    int status = QSPIF_BD_ERROR_OK;
    uint32_t offset = 0;
    uint32_t chunk = 0;
    bd_size_t written_bytes = 0;

    tr_debug("Program - Buff: 0x%lxh, addr: %llu, size: %llu", (uint32_t)buffer, addr, size);

    while (size > 0) {
        // Write on _page_size_bytes boundaries (Default 256 bytes a page)
        offset = addr % _page_size_bytes;
        chunk = (offset + size < _page_size_bytes) ? size : (_page_size_bytes - offset);
        written_bytes = chunk;

        _mutex.lock();

        //Send WREN
        if (_set_write_enable() != 0) {
            tr_error("Write Enabe failed");
            program_failed = true;
            status = QSPIF_BD_ERROR_WREN_FAILED;
            goto exit_point;
        }

        result = _qspi_send_program_command(_prog_instruction, buffer, addr, &written_bytes);
        if ((result != QSPI_STATUS_OK) || (chunk != written_bytes)) {
            tr_error("Write failed");
            program_failed = true;
            status = QSPIF_BD_ERROR_DEVICE_ERROR;
            goto exit_point;
        }

        buffer = static_cast<const uint8_t *>(buffer) + chunk;
        addr += chunk;
        size -= chunk;

        if (false == _is_mem_ready()) {
            tr_error("Device not ready after write, failed");
            program_failed = true;
            status = QSPIF_BD_ERROR_READY_FAILED;
            goto exit_point;
        }
        _mutex.unlock();
    }

exit_point:
    if (program_failed) {
        _mutex.unlock();
    }

    return status;
}

int QSPIFBlockDevice::erase(bd_addr_t addr, bd_size_t in_size)
{
    int type = 0;
    uint32_t offset = 0;
    uint32_t chunk = 4096;
    qspi_inst_t cur_erase_inst = _erase_instruction;
    int size = (int)in_size;
    bool erase_failed = false;
    int status = QSPIF_BD_ERROR_OK;
    // Find region of erased address
    int region = _utils_find_addr_region(addr);
    // Erase Types of selected region
    uint8_t bitfield = _region_erase_types_bitfield[region];

    tr_debug("Erase - addr: %llu, in_size: %llu", addr, in_size);

    if ((addr + in_size) > _device_size_bytes) {
        tr_error("Erase exceeds flash device size");
        return QSPIF_BD_ERROR_INVALID_ERASE_PARAMS;
    }

    if (((addr % get_erase_size(addr)) != 0) || (((addr + in_size) % get_erase_size(addr + in_size - 1)) != 0)) {
        tr_error("Invalid erase - unaligned address and size");
        return QSPIF_BD_ERROR_INVALID_ERASE_PARAMS;
    }

    // For each iteration erase the largest section supported by current region
    while (size > 0) {
        // iterate to find next Largest erase type ( a. supported by region, b. smaller than size)
        // find the matching instruction and erase size chunk for that type.
        type = _utils_iterate_next_largest_erase_type(bitfield, size, (int)addr, _region_high_boundary[region]);
        cur_erase_inst = _erase_type_inst_arr[type];
        offset = addr % _erase_type_size_arr[type];
        chunk = ((offset + size) < _erase_type_size_arr[type]) ? size : (_erase_type_size_arr[type] - offset);

        tr_debug("Erase - addr: %llu, size:%d, Inst: 0x%xh, chunk: %lu ",
                 addr, size, cur_erase_inst, chunk);
        tr_debug("Erase - Region: %d, Type:%d ",
                 region, type);

        _mutex.lock();

        if (_set_write_enable() != 0) {
            tr_error("QSPI Erase Device not ready - failed");
            erase_failed = true;
            status = QSPIF_BD_ERROR_WREN_FAILED;
            goto exit_point;
        }

        if (QSPI_STATUS_OK != _qspi_send_erase_command(cur_erase_inst, addr, size)) {
            tr_error("QSPI Erase command failed!");
            erase_failed = true;
            status = QSPIF_BD_ERROR_DEVICE_ERROR;
            goto exit_point;
        }

        addr += chunk;
        size -= chunk;

        if ((size > 0) && (addr > _region_high_boundary[region])) {
            // erase crossed to next region
            region++;
            bitfield = _region_erase_types_bitfield[region];
        }

        if (false == _is_mem_ready()) {
            tr_error("QSPI After Erase Device not ready - failed");
            erase_failed = true;
            status = QSPIF_BD_ERROR_READY_FAILED;
            goto exit_point;
        }

        _mutex.unlock();
    }

exit_point:
    if (erase_failed) {
        _mutex.unlock();
    }

    return status;
}

bd_size_t QSPIFBlockDevice::get_read_size() const
{
    // Return minimum read size in bytes for the device
    return MBED_CONF_QSPIF_QSPI_MIN_READ_SIZE;
}

bd_size_t QSPIFBlockDevice::get_program_size() const
{
    // Return minimum program/write size in bytes for the device
    return MBED_CONF_QSPIF_QSPI_MIN_PROG_SIZE;
}

bd_size_t QSPIFBlockDevice::get_erase_size() const
{
    // return minimal erase size supported by all regions (0 if none exists)
    return _min_common_erase_size;
}

const char *QSPIFBlockDevice::get_type() const
{
    return "QSPIF";
}

// Find minimal erase size supported by the region to which the address belongs to
bd_size_t QSPIFBlockDevice::get_erase_size(bd_addr_t addr)
{
    // Find region of current address
    int region = _utils_find_addr_region(addr);

    int min_region_erase_size = _min_common_erase_size;
    int8_t type_mask = ERASE_BITMASK_TYPE1;
    int i_ind = 0;


    if (region != -1) {
        type_mask = 0x01;

        for (i_ind = 0; i_ind < 4; i_ind++) {
            // loop through erase types bitfield supported by region
            if (_region_erase_types_bitfield[region] & type_mask) {

                min_region_erase_size = _erase_type_size_arr[i_ind];
                break;
            }
            type_mask = type_mask << 1;
        }

        if (i_ind == 4) {
            tr_error("No erase type was found for region addr");
        }
    }

    return (bd_size_t)min_region_erase_size;
}

bd_size_t QSPIFBlockDevice::size() const
{
    return _device_size_bytes;
}

int QSPIFBlockDevice::get_erase_value() const
{
    return 0xFF;
}

/********************************/
/*   Different Device Csel Mgmt */
/********************************/
static PinName *generate_initialized_active_qspif_csel_arr()
{
    PinName *init_arr = new PinName[QSPIF_MAX_ACTIVE_FLASH_DEVICES];
    for (int i_ind = 0; i_ind < QSPIF_MAX_ACTIVE_FLASH_DEVICES; i_ind++) {
        init_arr[i_ind] = NC;
    }
    return init_arr;
}

int QSPIFBlockDevice::add_new_csel_instance(PinName csel)
{
    int status = 0;
    _devices_mutex->lock();
    if (_number_of_active_qspif_flash_csel >= QSPIF_MAX_ACTIVE_FLASH_DEVICES) {
        status = -2;
        goto exit_point;
    }

    // verify the device is unique(no identical csel already exists)
    for (int i_ind = 0; i_ind < QSPIF_MAX_ACTIVE_FLASH_DEVICES; i_ind++) {
        if (_active_qspif_flash_csel_arr[i_ind] == csel) {
            status = -1;
            goto exit_point;
        }
    }

    // Insert new csel into existing device list
    for (int i_ind = 0; i_ind < QSPIF_MAX_ACTIVE_FLASH_DEVICES; i_ind++) {
        if (_active_qspif_flash_csel_arr[i_ind] == NC) {
            _active_qspif_flash_csel_arr[i_ind] = csel;
            break;
        }
    }
    _number_of_active_qspif_flash_csel++;

exit_point:
    _devices_mutex->unlock();
    return status;
}

int QSPIFBlockDevice::remove_csel_instance(PinName csel)
{
    int status = -1;
    _devices_mutex->lock();
    // remove the csel from existing device list
    for (int i_ind = 0; i_ind < QSPIF_MAX_ACTIVE_FLASH_DEVICES; i_ind++) {
        if (_active_qspif_flash_csel_arr[i_ind] == csel) {
            _active_qspif_flash_csel_arr[i_ind] = NC;
            if (_number_of_active_qspif_flash_csel > 0) {
                _number_of_active_qspif_flash_csel--;
            }
            status = 0;
            break;
        }
    }
    _devices_mutex->unlock();
    return status;
}

/*********************************************************/
/********** SFDP Parsing and Detection Functions *********/
/*********************************************************/
int QSPIFBlockDevice::_sfdp_parse_sfdp_headers(uint32_t &basic_table_addr, size_t &basic_table_size,
                                               uint32_t &sector_map_table_addr, size_t &sector_map_table_size)
{
    uint8_t sfdp_header[QSPIF_SFDP_HEADER_SIZE];
    uint8_t param_header[QSPIF_PARAM_HEADER_SIZE];
    size_t data_length = QSPIF_SFDP_HEADER_SIZE;
    bd_addr_t addr = 0x0;

    qspi_status_t status = _qspi_send_read_sfdp_command(addr, (char*) sfdp_header, data_length);
    if (status != QSPI_STATUS_OK) {
        tr_error("Init - Read SFDP Failed");
        return -1;
    }

    // Verify SFDP signature for sanity
    // Also check that major/minor version is acceptable
    if (!(memcmp(&sfdp_header[0], "SFDP", 4) == 0 && sfdp_header[5] == 1)) {
        tr_error("Init - _verify SFDP signature and version Failed");
        return -1;
    } else {
        tr_debug("Init - verified SFDP Signature and version Successfully");
    }

    // Discover Number of Parameter Headers
    int number_of_param_headers = (int)(sfdp_header[6]) + 1;
    tr_debug("Number of Param Headers: %d", number_of_param_headers);


    addr += QSPIF_SFDP_HEADER_SIZE;
    data_length = QSPIF_PARAM_HEADER_SIZE;

    // Loop over Param Headers and parse them (currently supported Basic Param Table and Sector Region Map Table)
    for (int i_ind = 0; i_ind < number_of_param_headers; i_ind++) {
        status = _qspi_send_read_sfdp_command(addr, (char *) param_header, data_length);
        if (status != QSPI_STATUS_OK) {
            tr_error("Init - Read Param Table %d Failed", i_ind + 1);
            return -1;
        }

        // The SFDP spec indicates the standard table is always at offset 0
        // in the parameter headers, we check just to be safe
        if (param_header[2] != 1) {
            tr_error("Param Table %d - Major Version should be 1!", i_ind + 1);
            return -1;
        }

        if ((param_header[0] == 0) && (param_header[7] == 0xFF)) {
            // Found Basic Params Table: LSB=0x00, MSB=0xFF
            tr_debug("Found Basic Param Table at Table: %d", i_ind + 1);
            basic_table_addr = ((param_header[6] << 16) | (param_header[5] << 8) | (param_header[4]));
            // Supporting up to 64 Bytes Table (16 DWORDS)
            basic_table_size = ((param_header[3] * 4) < SFDP_DEFAULT_BASIC_PARAMS_TABLE_SIZE_BYTES) ? (param_header[3] * 4) : 64;

        } else if ((param_header[0] == 81) && (param_header[7] == 0xFF)) {
            // Found Sector Map Table: LSB=0x81, MSB=0xFF
            tr_debug("Found Sector Map Table at Table: %d", i_ind + 1);
            sector_map_table_addr = ((param_header[6] << 16) | (param_header[5] << 8) | (param_header[4]));
            sector_map_table_size = param_header[3] * 4;

        }
        addr += QSPIF_PARAM_HEADER_SIZE;

    }
    return 0;
}


int QSPIFBlockDevice::_sfdp_parse_basic_param_table(uint32_t basic_table_addr, size_t basic_table_size)
{
    uint8_t param_table[SFDP_DEFAULT_BASIC_PARAMS_TABLE_SIZE_BYTES]; /* Up To 16 DWORDS = 64 Bytes */

    qspi_status_t status = _qspi_send_read_sfdp_command(basic_table_addr, (char*) param_table, basic_table_size);
    if (status != QSPI_STATUS_OK) {
        tr_error("Init - Read SFDP First Table Failed");
        return -1;
    }

    // Check that density is not greater than 4 gigabits (i.e. that addressing beyond 4 bytes is not required)
    if ((param_table[7] & 0x80) != 0) {
        tr_error("Init - verify flash density failed");
        return -1;
    }

    // Get device density (stored in bits - 1)
    uint32_t density_bits = (
                                (param_table[7] << 24) |
                                (param_table[6] << 16) |
                                (param_table[5] << 8) |
                                param_table[4]);
    _device_size_bytes = (density_bits + 1) / 8;

    // Set Default read/program/erase Instructions
    _read_instruction = QSPIF_READ;
    _prog_instruction = QSPIF_PP;
    _erase_instruction = QSPIF_SE;

    _erase_instruction = _erase4k_inst;

    // Set Page Size (QSPI write must be done on Page limits)
    _page_size_bytes = _sfdp_detect_page_size(param_table, basic_table_size);

    if (_sfdp_detect_reset_protocol_and_reset(param_table) != QSPIF_BD_ERROR_OK) {
        tr_error("Init - Detecting reset protocol/resetting failed");
        return -1;
    }

    // Detect and Set Erase Types
    bool shouldSetQuadEnable = false;
    bool is_qpi_mode = false;

    _sfdp_detect_erase_types_inst_and_size(param_table, basic_table_size, _erase4k_inst, _erase_type_inst_arr,
                                           _erase_type_size_arr);
    _erase_instruction = _erase4k_inst;

    // Detect and Set fastest Bus mode (default 1-1-1)
    _sfdp_detect_best_bus_read_mode(param_table, basic_table_size, shouldSetQuadEnable, is_qpi_mode, _read_instruction);
    if (true == shouldSetQuadEnable) {
        // Set Quad Enable and QPI Bus modes if Supported
        tr_debug("Init - Setting Quad Enable");
        if (0 != _sfdp_set_quad_enabled(param_table)) {
            tr_error("Device supports Quad bus, but Quad Enable Failed");
            return -1;
        }
        if (true == is_qpi_mode) {
            tr_debug("Init - Setting QPI mode");
            _sfdp_set_qpi_enabled(param_table);
        }
    }

    if (_sfdp_detect_and_enable_4byte_addressing(param_table, basic_table_size) != QSPIF_BD_ERROR_OK) {
        tr_error("Init - Detecting/enabling 4-byte addressing failed");
        return -1;
    }

    if (false == _is_mem_ready()) {
        tr_error("Init - _is_mem_ready Failed");
        return -1;
    }

    return 0;
}

int QSPIFBlockDevice::_sfdp_set_quad_enabled(uint8_t *basic_param_table_ptr)
{
    uint8_t status_reg_setup[QSPI_STATUS_REGISTER_COUNT] = {0};
    uint8_t status_regs[QSPI_STATUS_REGISTER_COUNT] = {0};

    // QUAD Enable procedure is specified by 3 bits
    uint8_t qer_value = (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_QER_BYTE] & 0x70) >> 4;

    switch (qer_value) {
        case 0:
            tr_debug("Device Does not Have a QE Bit, continue based on Read Inst");
            return 0;
        case 1:
        case 4:
            status_reg_setup[1] = 1 << 1;  // Bit 1 of Status Reg 2
            tr_debug("Setting QE Bit, Bit 1 of Status Reg 2");
            break;
        case 2:
            status_reg_setup[0] = 1 << 6; // Bit 6 of Status Reg 1
            tr_debug("Setting QE Bit, Bit 6 of Status Reg 1");
            break;
        case 3:
            status_reg_setup[0] = 1 << 7; // Bit 7 of Status Reg 1
            _write_status_reg_2_inst = 0x3E;
            _read_status_reg_2_inst = 0x3F;
            tr_debug("Setting QE Bit, Bit 7 of Status Reg 1");
            break;
        case 5:
            status_reg_setup[1] = 1 << 1; // Bit 1 of status Reg 2
            tr_debug("Setting QE Bit, Bit 1 of Status Reg 2");
            break;
        default:
            tr_warning("Unsuported QER configuration");
            return 0;
    }

    // Read existing status register values
    _qspi_read_status_registers(status_regs);

    // Set Bits for Quad Enable
    for (int i = 0; i < QSPI_STATUS_REGISTER_COUNT; i++) {
        status_regs[i] |= status_reg_setup[i];
    }

    // Write new Status Register Setup
    _qspi_write_status_registers(status_regs);

    if (false == _is_mem_ready()) {
        tr_error("Device not ready after write, failed");
        return -1;
    }

    // For Debug
    memset(status_regs, 0, QSPI_STATUS_REGISTER_COUNT);
    _qspi_read_status_registers(status_regs);
    if (((status_regs[0] & status_reg_setup[0]) | (status_regs[1] & status_reg_setup[1])) == 0) {
        tr_error("Status register not set correctly");
        return -1;
    }

    return 0;
}

int QSPIFBlockDevice::_sfdp_set_qpi_enabled(uint8_t *basic_param_table_ptr)
{
    uint8_t config_reg[1];

    // QPI 4-4-4 Enable Procedure is specified in 5 Bits
    uint8_t en_seq_444_value = (((basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_444_MODE_EN_SEQ_BYTE] & 0xF0) >> 4) | ((
                                                                                                                           basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_444_MODE_EN_SEQ_BYTE 
    switch (en_seq_444_value) {
        case 1:
        case 2:
            tr_debug("_sfdp_set_qpi_enabled - send command 38h");
            if (QSPI_STATUS_OK != _qspi_send_general_command(0x38, QSPI_NO_ADDRESS_COMMAND, NULL, 0, NULL, 0)) {
                tr_error("_sfdp_set_qpi_enabled - send command 38h Failed");
            }
            break;

        case 4:
            tr_debug("_sfdp_set_qpi_enabled - send command 35h");
            if (QSPI_STATUS_OK != _qspi_send_general_command(0x35, QSPI_NO_ADDRESS_COMMAND, NULL, 0, NULL, 0)) {
                tr_error("_sfdp_set_qpi_enabled - send command 35h Failed");
            }
            break;

        case 8:
            tr_debug("_sfdp_set_qpi_enabled - set config bit 6 and send command 71h");
            if (QSPI_STATUS_OK != _qspi_send_general_command(0x65, 0x800003, NULL, 0, (char *)config_reg, 1)) {
                tr_error("_sfdp_set_qpi_enabled - set config bit 6 command 65h Failed");
            }
            config_reg[0] |= 0x40; //Set Bit 6
            if (QSPI_STATUS_OK != _qspi_send_general_command(0x71, 0x800003, NULL, 0, (char *)config_reg, 1)) {
                tr_error("_sfdp_set_qpi_enabled - send command 71h Failed");
            }
            break;

        case 16:
            tr_debug("_sfdp_set_qpi_enabled - reset config bits 0-7 and send command 61h");
            if (QSPI_STATUS_OK != _qspi_send_general_command(0x65, QSPI_NO_ADDRESS_COMMAND, NULL, 0, (char *)config_reg, 1)) {
                tr_error("_sfdp_set_qpi_enabled - send command 65h Failed");
            }
            config_reg[0] &= 0x7F; //Reset Bit 7 of CR
            if (QSPI_STATUS_OK != _qspi_send_general_command(0x61, QSPI_NO_ADDRESS_COMMAND, NULL, 0, (char *)config_reg, 1)) {
                tr_error("_sfdp_set_qpi_enabled - send command 61 Failed");
            }
            break;

        default:
            tr_warning("_sfdp_set_qpi_enabled - Unsuported En Seq 444 configuration");
            break;
    }
    return 0;
}

int QSPIFBlockDevice::_sfdp_detect_page_size(uint8_t *basic_param_table_ptr, int basic_param_table_size)
{
    unsigned int page_size = QSPIF_DEFAULT_PAGE_SIZE;

    if (basic_param_table_size > QSPIF_BASIC_PARAM_TABLE_PAGE_SIZE_BYTE) {
        // Page Size is specified by 4 Bits (N), calculated by 2^N
        int page_to_power_size = ((int)basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_PAGE_SIZE_BYTE]) >> 4;
        page_size = local_math_power(2, page_to_power_size);
        tr_debug("Detected Page Size: %d", page_size);
    } else {
        tr_debug("Using Default Page Size: %d", page_size);
    }
    return page_size;
}

int QSPIFBlockDevice::_sfdp_detect_erase_types_inst_and_size(uint8_t *basic_param_table_ptr, int basic_param_table_size,
                                                             qspi_inst_t &erase4k_inst,
                                                             qspi_inst_t *erase_type_inst_arr, unsigned int *erase_type_size_arr)
{
    erase4k_inst = 0xff;
    bool found_4Kerase_type = false;
    uint8_t bitfield = 0x01;

    // Erase 4K Inst is taken either from param table legacy 4K erase or superseded by erase Instruction for type of size 4K
    erase4k_inst = basic_param_table_ptr[QSPIF_BASIC_PARAM_4K_ERASE_TYPE_BYTE];

    if (basic_param_table_size > QSPIF_BASIC_PARAM_ERASE_TYPE_1_SIZE_BYTE) {
        // Loop Erase Types 1-4
        for (int i_ind = 0; i_ind < 4; i_ind++) {
            erase_type_inst_arr[i_ind] = 0xff; //0xFF default for unsupported type
            erase_type_size_arr[i_ind] = local_math_power(2,
                                                          basic_param_table_ptr[QSPIF_BASIC_PARAM_ERASE_TYPE_1_SIZE_BYTE + 2 * i_ind]); // Size given as 2^N
            tr_debug("Erase Type(A) %d - Inst: 0x%xh, Size: %d", (i_ind + 1), erase_type_inst_arr[i_ind],
                     erase_type_size_arr[i_ind]);
            if (erase_type_size_arr[i_ind] > 1) {
                // if size==1 type is not supported
                erase_type_inst_arr[i_ind] = basic_param_table_ptr[QSPIF_BASIC_PARAM_ERASE_TYPE_1_BYTE + 2 * i_ind];

                if ((erase_type_size_arr[i_ind] < _min_common_erase_size) || (_min_common_erase_size == 0)) {
                    //Set default minimal common erase for singal region
                    _min_common_erase_size = erase_type_size_arr[i_ind];
                }

                // SFDP standard requires 4K Erase type to exist and its instruction to be identical to legacy field erase instruction
                if (erase_type_size_arr[i_ind] == 4096) {
                    found_4Kerase_type = true;
                    if (erase4k_inst != erase_type_inst_arr[i_ind]) {
                        //Verify 4KErase Type is identical to Legacy 4K erase type specified in Byte 1 of Param Table
                        erase4k_inst = erase_type_inst_arr[i_ind];
                        tr_warning("_detectEraseTypesInstAndSize - Default 4K erase Inst is different than erase type Inst for 4K");

                    }
                }
                _region_erase_types_bitfield[0] |= bitfield; // If there's no region map, set region "0" types bitfield as defualt;
            }

            tr_debug("Erase Type %d - Inst: 0x%xh, Size: %d", (i_ind + 1), erase_type_inst_arr[i_ind],
                     erase_type_size_arr[i_ind]);
            bitfield = bitfield << 1;
        }
    }

    if (false == found_4Kerase_type) {
        tr_warning("Couldn't find Erase Type for 4KB size");
    }
    return 0;
}

int QSPIFBlockDevice::_sfdp_detect_best_bus_read_mode(uint8_t *basic_param_table_ptr, int basic_param_table_size,
                                                      bool &set_quad_enable,
                                                      bool &is_qpi_mode, qspi_inst_t &read_inst)
{
    set_quad_enable = false;
    is_qpi_mode = false;
    uint8_t examined_byte;

    do { // compound statement is the loop body

        if (basic_param_table_size > QSPIF_BASIC_PARAM_TABLE_QPI_READ_SUPPORT_BYTE) {
            examined_byte = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_QPI_READ_SUPPORT_BYTE];

            if (examined_byte & 0x10) {
                // QPI 4-4-4 Supported
                read_inst = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_444_READ_INST_BYTE];
                set_quad_enable = true;
                is_qpi_mode = true;
                _dummy_and_mode_cycles = (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_444_READ_INST_BYTE - 1] >> 5)
                                         + (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_444_READ_INST_BYTE - 1] & 0x1F);
                tr_debug("Read Bus Mode set to 4-4-4, Instruction: 0x%xh", _read_instruction);
                //_inst_width = QSPI_CFG_BUS_QUAD;
                _address_width = QSPI_CFG_BUS_QUAD;
                _data_width = QSPI_CFG_BUS_QUAD;
            }
        }
        is_qpi_mode = false;
        examined_byte = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_FAST_READ_SUPPORT_BYTE];
        if (examined_byte & 0x20) {
            //  Fast Read 1-4-4 Supported
            read_inst = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_144_READ_INST_BYTE];
            set_quad_enable = true;
            // dummy cycles + mode cycles = Dummy Cycles
            _dummy_and_mode_cycles = (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_144_READ_INST_BYTE - 1] >> 5)
                                     + (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_144_READ_INST_BYTE - 1] & 0x1F);
            _address_width = QSPI_CFG_BUS_QUAD;
            _data_width = QSPI_CFG_BUS_QUAD;
            tr_debug("Read Bus Mode set to 1-4-4, Instruction: 0x%xh", _read_instruction);
            break;
        }

        if (examined_byte & 0x40) {
            //  Fast Read 1-1-4 Supported
            read_inst = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_114_READ_INST_BYTE];
            set_quad_enable = true;
            _dummy_and_mode_cycles = (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_114_READ_INST_BYTE - 1] >> 5)
                                     + (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_114_READ_INST_BYTE - 1] & 0x1F);
            _data_width = QSPI_CFG_BUS_QUAD;
            tr_debug("Read Bus Mode set to 1-1-4, Instruction: 0x%xh", _read_instruction);
            break;
        }
        examined_byte = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_QPI_READ_SUPPORT_BYTE];
        if (examined_byte & 0x01) {
            //  Fast Read 2-2-2 Supported
            read_inst = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_222_READ_INST_BYTE];
            _dummy_and_mode_cycles = (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_222_READ_INST_BYTE - 1] >> 5)
                                     + (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_222_READ_INST_BYTE - 1] & 0x1F);
            _address_width = QSPI_CFG_BUS_DUAL;
            _data_width = QSPI_CFG_BUS_DUAL;
            tr_debug("Read Bus Mode set to 2-2-2, Instruction: 0x%xh", _read_instruction);
            break;
        }

        examined_byte = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_FAST_READ_SUPPORT_BYTE];
        if (examined_byte & 0x10) {
            //  Fast Read 1-2-2 Supported
            read_inst = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_122_READ_INST_BYTE];
            _dummy_and_mode_cycles = (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_122_READ_INST_BYTE - 1] >> 5)
                                     + (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_122_READ_INST_BYTE - 1] & 0x1F);
            _address_width = QSPI_CFG_BUS_DUAL;
            _data_width = QSPI_CFG_BUS_DUAL;
            tr_debug("Read Bus Mode set to 1-2-2, Instruction: 0x%xh", _read_instruction);
            break;
        }
        if (examined_byte & 0x01) {
            // Fast Read 1-1-2 Supported
            read_inst = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_112_READ_INST_BYTE];
            _dummy_and_mode_cycles = (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_112_READ_INST_BYTE - 1] >> 5)
                                     + (basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_112_READ_INST_BYTE - 1] & 0x1F);
            _data_width = QSPI_CFG_BUS_DUAL;
            tr_debug("Read Bus Mode set to 1-1-2, Instruction: 0x%xh", _read_instruction);
            break;
        }
        tr_debug("Read Bus Mode set to 1-1-1, Instruction: 0x%xh", _read_instruction);
    } while (false);

    return 0;
}

int QSPIFBlockDevice::_sfdp_detect_and_enable_4byte_addressing(uint8_t *basic_param_table_ptr, int basic_param_table_size)
{
    int status = QSPIF_BD_ERROR_OK;
    qspi_status_t qspi_status = QSPI_STATUS_OK;

    // Always enable 4-byte addressing if possible
    if (basic_param_table_size > QSPIF_BASIC_PARAM_TABLE_4BYTE_ADDR_BYTE) {
        uint8_t examined_byte = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_4BYTE_ADDR_BYTE];

        if (examined_byte & FOURBYTE_ADDR_ALWAYS_BITMASK) {
            // No need to do anything if 4-byte addressing is always enabled
            _address_size = QSPI_CFG_ADDR_SIZE_32;
        } else if (examined_byte & FOURBYTE_ADDR_B7_BITMASK) {
            // Issue instruction B7h to enable 4-byte addressing
            qspi_status = _qspi_send_general_command(0xB7, QSPI_NO_ADDRESS_COMMAND, NULL, 0, NULL, 0);
            status = (qspi_status == QSPI_STATUS_OK) ? QSPIF_BD_ERROR_OK : QSPIF_BD_ERROR_PARSING_FAILED;
            if (status == QSPIF_BD_ERROR_OK) {
                _address_size = QSPI_CFG_ADDR_SIZE_32;
            }
        } else if (examined_byte & FOURBYTE_ADDR_B7_WREN_BITMASK) {
            // Issue WREN and then instruction B7h to enable 4-byte addressing
            if (_set_write_enable() == 0) {
                qspi_status = _qspi_send_general_command(0xB7, QSPI_NO_ADDRESS_COMMAND, NULL, 0, NULL, 0);
                status = (qspi_status == QSPI_STATUS_OK) ? QSPIF_BD_ERROR_OK : QSPIF_BD_ERROR_PARSING_FAILED;

                if (status == QSPIF_BD_ERROR_OK) {
                    _address_size = QSPI_CFG_ADDR_SIZE_32;
                }
            } else {
                tr_error("Write enable failed");
                status = QSPIF_BD_ERROR_WREN_FAILED;
            }
        } else if (examined_byte & FOURBYTE_ADDR_CONF_REG_BITMASK) {
            // Write 1 to bit 0 of a configuration register to enable 4-byte addressing
            // Write to register with instruction B1h, read from register with instruction B5h
            uint8_t conf_register = 0;
            qspi_status = _qspi_send_general_command(0xB5, QSPI_NO_ADDRESS_COMMAND, NULL, 0, (char *) &conf_register, 1);
            status = (qspi_status == QSPI_STATUS_OK) ? QSPIF_BD_ERROR_OK : QSPIF_BD_ERROR_PARSING_FAILED;

            if (status == QSPIF_BD_ERROR_OK) {
                conf_register |= 0b00000001;
                if (_set_write_enable() == 0) {
                    qspi_status_t qspi_status = _qspi_send_general_command(0xB1, QSPI_NO_ADDRESS_COMMAND, (char *) &conf_register, 1, NULL, 0);
                    status = (qspi_status == QSPI_STATUS_OK) ? QSPIF_BD_ERROR_OK : QSPIF_BD_ERROR_PARSING_FAILED;

                    if (status == QSPIF_BD_ERROR_OK) {
                        _address_size = QSPI_CFG_ADDR_SIZE_32;
                    }
                } else {
                    tr_error("Write enable failed");
                    status = QSPIF_BD_ERROR_WREN_FAILED;
                }
            }
        } else if (examined_byte & FOURBYTE_ADDR_BANK_REG_BITMASK) {
            // Write 1 to bit 7 of a bank register to enable 4-byte addressing
            // Write to register with instruction 17h, read from register with instruction 16h
            uint8_t to_write = 0b10000000;
            qspi_status = _qspi_send_general_command(0x17, QSPI_NO_ADDRESS_COMMAND, (char *) &to_write, 1, NULL, 0);
            status = (qspi_status == QSPI_STATUS_OK) ? QSPIF_BD_ERROR_OK : QSPIF_BD_ERROR_PARSING_FAILED;
            if (status == QSPIF_BD_ERROR_OK) {
                _address_size = QSPI_CFG_ADDR_SIZE_32;
            }
        } else if (examined_byte & FOURBYTE_ADDR_EXT_ADDR_REG_BITMASK) {
            // Extended address register stores most significant byte of a 4-byte address
            // Instructions are sent with the lower 3 bytes of the address
            // Write to register with instruction C5h, read from register with instruction C8h
            _4byte_msb_reg_write_inst = 0xC5;
            _address_size = QSPI_CFG_ADDR_SIZE_24;
        } else {
            // Either part specific instructions are required to use 4-byte addressing or it isn't supported, so use 3-byte addressing instead
            tr_debug("_sfdp_detect_and_enable_4byte_addressing - 4-byte addressing not supported, falling back to 3-byte addressing");
            _address_size = QSPI_CFG_ADDR_SIZE_24;
        }
    }

    return status;
}

int QSPIFBlockDevice::_sfdp_detect_reset_protocol_and_reset(uint8_t *basic_param_table_ptr)
{
    int status = QSPIF_BD_ERROR_OK;
    uint8_t examined_byte = basic_param_table_ptr[QSPIF_BASIC_PARAM_TABLE_SOFT_RESET_BYTE];

    // Ignore bit indicating need to exit 0-4-4 mode - should not enter 0-4-4 mode from QSPIFBlockDevice
    if (examined_byte & SOFT_RESET_RESET_INST_BITMASK) {
        // Issue instruction 0xF0 to reset the device
        qspi_status_t qspi_status = _qspi_send_general_command(0xF0, QSPI_NO_ADDRESS_COMMAND, // Send reset instruction
                                                               NULL, 0, NULL, 0);
        status = (qspi_status == QSPI_STATUS_OK) ? QSPIF_BD_ERROR_OK : QSPIF_BD_ERROR_PARSING_FAILED;
    } else if (examined_byte & SOFT_RESET_ENABLE_AND_RESET_INST_BITMASK) {
        // Issue instruction 66h to enable resets on the device
        // Then issue instruction 99h to reset the device
        qspi_status_t qspi_status = _qspi_send_general_command(0x66, QSPI_NO_ADDRESS_COMMAND, // Send reset enable instruction
                                                                NULL, 0, NULL, 0);
        if (qspi_status == QSPI_STATUS_OK) {
            qspi_status = _qspi_send_general_command(0x99, QSPI_NO_ADDRESS_COMMAND, // Send reset instruction
                                                     NULL, 0, NULL, 0);
        }
        status = (qspi_status == QSPI_STATUS_OK) ? QSPIF_BD_ERROR_OK : QSPIF_BD_ERROR_PARSING_FAILED;
    } else {
        // Soft reset either is not supported or requires direct control over data lines
        status = QSPIF_BD_ERROR_PARSING_FAILED;
    }

    if (status == QSPIF_BD_ERROR_OK){
        if (false == _is_mem_ready()) {
            tr_error("Device not ready, reset failed");
            status = QSPIF_BD_ERROR_READY_FAILED;
        }
    }

    return status;
}

int QSPIFBlockDevice::_sfdp_parse_sector_map_table(uint32_t sector_map_table_addr, size_t sector_map_table_size)
{
    uint8_t sector_map_table[SFDP_DEFAULT_BASIC_PARAMS_TABLE_SIZE_BYTES]; /* Up To 16 DWORDS = 64 Bytes */
    uint32_t tmp_region_size = 0;
    int i_ind = 0;
    int prev_boundary = 0;
    // Default set to all type bits 1-4 are common
    int min_common_erase_type_bits = ERASE_BITMASK_ALL;

    qspi_status_t status = _qspi_send_read_sfdp_command(sector_map_table_addr, (char *) sector_map_table, sector_map_table_size);
    if (status != QSPI_STATUS_OK) {
        tr_error("Init - Read SFDP First Table Failed");
        return -1;
    }

    // Currently we support only Single Map Descriptor
    if (!((sector_map_table[0] & 0x3) == 0x03) && (sector_map_table[1]  == 0x0)) {
        tr_error("Sector Map - Supporting Only Single! Map Descriptor (not map commands)");
        return -1;
    }

    _regions_count = sector_map_table[2] + 1;
    if (_regions_count > QSPIF_MAX_REGIONS) {
        tr_error("Supporting up to %d regions, current setup to %d regions - fail",
                 QSPIF_MAX_REGIONS, _regions_count);
        return -1;
    }

    // Loop through Regions and set for each one: size, supported erase types, high boundary offset
    // Calculate minimum Common Erase Type for all Regions
    for (i_ind = 0; i_ind < _regions_count; i_ind++) {
        tmp_region_size = ((*((uint32_t *)&sector_map_table[(i_ind + 1) * 4])) >> 8) & 0x00FFFFFF; // bits 9-32
        _region_size_bytes[i_ind] = (tmp_region_size + 1) * 256; // Region size is 0 based multiple of 256 bytes;
        _region_erase_types_bitfield[i_ind] = sector_map_table[(i_ind + 1) * 4] & 0x0F; // bits 1-4
        min_common_erase_type_bits &= _region_erase_types_bitfield[i_ind];
        _region_high_boundary[i_ind] = (_region_size_bytes[i_ind] - 1) + prev_boundary;
        prev_boundary = _region_high_boundary[i_ind] + 1;
    }

    // Calc minimum Common Erase Size from min_common_erase_type_bits
    uint8_t type_mask = ERASE_BITMASK_TYPE1;
    for (i_ind = 0; i_ind < 4; i_ind++) {
        if (min_common_erase_type_bits & type_mask) {
            _min_common_erase_size = _erase_type_size_arr[i_ind];
            break;
        }
        type_mask = type_mask << 1;
    }

    if (i_ind == 4) {
        // No common erase type was found between regions
        _min_common_erase_size = 0;
    }

    return 0;
}

int QSPIFBlockDevice::_clear_block_protection()
{
    uint8_t vendor_device_ids[QSPI_RDID_DATA_LENGTH] = {0};
    uint8_t status_regs[QSPI_STATUS_REGISTER_COUNT] = {0};

    if (false == _is_mem_ready()) {
        tr_error("Device not ready, clearing block protection failed");
        return -1;
    }

    /* Read Manufacturer ID (1byte), and Device ID (2bytes) */
    qspi_status_t status = _qspi_send_general_command(QSPIF_INST_RDID, QSPI_NO_ADDRESS_COMMAND,
                                             NULL, 0,
                                             (char *) vendor_device_ids, QSPI_RDID_DATA_LENGTH);
    if (QSPI_STATUS_OK != status) {
        tr_error("Read Vendor ID Failed");
        return -1;
    }

    tr_debug("Vendor device ID = 0x%x 0x%x 0x%x \n", vendor_device_ids[0], vendor_device_ids[1], vendor_device_ids[2]);
    switch (vendor_device_ids[0]) {
        case 0xbf:
            // SST devices come preset with block protection
            // enabled for some regions, issue global protection unlock to clear
            if (0 != _set_write_enable()) {
                tr_error("Write enable failed");
                return -1;
            }
            status = _qspi_send_general_command(QSPIF_INST_ULBPR, QSPI_NO_ADDRESS_COMMAND, NULL, 0, NULL, 0);
            if (QSPI_STATUS_OK != status) {
                tr_error("Global block protection unlock failed");
                return -1;
            }
            break;
        default:
            // For all other devices, clear all bits in status register 1 that aren't the WIP or WEL bits to clear the block protection bits
            status = _qspi_read_status_registers(status_regs);
            if (QSPI_STATUS_OK != status) {
                tr_error("_clear_block_protection - Status register read failed");
                return -1;
            }
            status_regs[0] &= (QSPIF_STATUS_BIT_WIP | QSPIF_STATUS_BIT_WEL);
            status = _qspi_write_status_registers(status_regs);
            if (QSPI_STATUS_OK != status) {
                tr_error("__clear_block_protection - Status register write failed");
                return -1;
            }
            break;
    }

    if (false == _is_mem_ready()) {
        tr_error("Device not ready, clearing block protection failed");
        return -1;
    }

    return 0;
}

int QSPIFBlockDevice::_set_write_enable()
{
    // Check Status Register Busy Bit to Verify the Device isn't Busy
    uint8_t status_value = 0;
    int status = -1;

    do {
        if (QSPI_STATUS_OK !=  _qspi_send_general_command(QSPIF_WREN, QSPI_NO_ADDRESS_COMMAND, NULL, 0, NULL, 0)) {
            tr_error("Sending WREN command FAILED");
            break;
        }

        if (false == _is_mem_ready()) {
            tr_error("Device not ready, write failed");
            break;
        }

        if (QSPI_STATUS_OK != _qspi_send_general_command(QSPIF_INST_RSR1, QSPI_NO_ADDRESS_COMMAND,
                                                         NULL, 0,
                                                         (char *) &status_value, 1)) {
            tr_error("Reading Status Register 1 failed");
            break;
        }

        if ((status_value & QSPIF_STATUS_BIT_WEL) == 0) {
            tr_error("_set_write_enable failed - status register 1 value: %u", status_value);
            break;
        }

        status = 0;
    } while (false);

    return status;
}

bool QSPIFBlockDevice::_is_mem_ready()
{
    // Check Status Register Busy Bit to Verify the Device isn't Busy
    uint8_t status_value = 0;
    int retries = 0;
    bool mem_ready = true;

    do {
        rtos::ThisThread::sleep_for(1);
        retries-        //Read the Status Register from device
        memset(status_value, 0xFF, QSPI_MAX_STATUS_REGISTER_SIZE);
        if (QSPI_STATUS_OK != _qspi_send_general_command(QSPIF_INST_RSR1, QSPI_NO_ADDRESS_COMMAND, NULL, 0, (char *) &status_value,
                                                         1)) {   // store received values in status_value
            tr_error("Reading Status Register failed");
        }
    } while ((status_value & QSPIF_STATUS_BIT_WIP) != 0 && retries < IS_MEM_READY_MAX_RETRIES);

    if ((status_value & QSPIF_STATUS_BIT_WIP) != 0) {
        tr_error("_is_mem_ready FALSE: status value = 0x%x ", status_value);
        mem_ready = false;
    }
    return mem_ready;
}

/*********************************************/
/************* Utility Functions *************/
/*********************************************/
int QSPIFBlockDevice::_utils_find_addr_region(bd_size_t offset)
{
    //Find the region to which the given offset belong to
    if ((offset > _device_size_bytes) || (_regions_count == 0)) {
        return -1;
    }

    if (_regions_count == 1) {
        return 0;
    }

    for (int i_ind = _regions_count - 2; i_ind >= 0; i_ind--) {

        if (offset > _region_high_boundary[i_ind]) {
            return (i_ind + 1);
        }
    }
    return -1;

}

int QSPIFBlockDevice::_utils_iterate_next_largest_erase_type(uint8_t &bitfield, int size, int offset, int boundry)
{
    // Iterate on all supported Erase Types of the Region to which the offset belong to.
    // Iterates from highest type to lowest
    uint8_t type_mask = ERASE_BITMASK_TYPE4;
    int i_ind  = 0;
    int largest_erase_type = 0;
    for (i_ind = 3; i_ind >= 0; i_ind--) {
        if (bitfield & type_mask) {
            largest_erase_type = i_ind;
            if ((size > (int)(_erase_type_size_arr[largest_erase_type])) &&
                    ((boundry - offset) > (int)(_erase_type_size_arr[largest_erase_type]))) {
                break;
            } else {
                bitfield &= ~type_mask;
            }
        }
        type_mask = type_mask >> 1;
    }

    if (i_ind == 4) {
        tr_error("No erase type was found for current region addr");
    }
    return largest_erase_type;

}

/***************************************************/
/*********** QSPI Driver API Functions *************/
/***************************************************/
qspi_status_t QSPIFBlockDevice::_qspi_set_frequency(int freq)
{
    return _qspi.set_frequency(freq);
}

qspi_status_t QSPIFBlockDevice::_qspi_update_4byte_ext_addr_reg(bd_addr_t addr)
{
    qspi_status_t status = QSPI_STATUS_OK;
    // Only update register if in the extended address register mode
    if (_4byte_msb_reg_write_inst != QSPI_NO_INST) {
        // Set register to the most significant byte of the address
        uint8_t most_significant_byte = addr >> 24;
        if (_set_write_enable() == 0) {
            status = _qspi.command_transfer(_4byte_msb_reg_write_inst, (int) QSPI_NO_ADDRESS_COMMAND,
                                            (char *) &most_significant_byte, 1,
                                            NULL, 0);
        } else {
            tr_error("Write enable failed");
            status = QSPI_STATUS_ERROR;
        }
    } else if ((_address_size != QSPI_CFG_ADDR_SIZE_32) && (addr != QSPI_NO_ADDRESS_COMMAND) && (addr >= (1 << 24))) {
        tr_error("Attempted to use 4-byte address but 4-byte addressing is not supported");
        status = QSPI_STATUS_ERROR;
    }
    return status;
}

qspi_status_t QSPIFBlockDevice::_qspi_send_read_command(qspi_inst_t read_inst, void *buffer, bd_addr_t addr,
                                                        bd_size_t size)
{
    size_t buf_len = size;

    qspi_status_t status = _qspi_update_4byte_ext_addr_reg(addr);
    if (QSPI_STATUS_OK != status) {
        tr_error("QSPI Read - Updating 4-byte addressing extended address register failed");
        return status;
    }

    // Send read command to device driver
    // Read commands use the best bus mode supported by the part
    _qspi.configure_format(_inst_width, _address_width, _address_size, QSPI_CFG_BUS_SINGLE, // Alt width should be the same as address width
                           QSPI_CFG_ALT_SIZE_8, _data_width, _dummy_and_mode_cycles);
    status = _qspi.read(read_inst, -1, (unsigned int)addr, (char *)buffer, &buf_len);
    // All commands other than Read and RSFDP use default 1-1-1 bus mode (Program/Erase are constrained by flash memory performance more than bus performance)
    _qspi.configure_format(QSPI_CFG_BUS_SINGLE, QSPI_CFG_BUS_SINGLE, _address_size, QSPI_CFG_BUS_SINGLE,
                           QSPI_CFG_ALT_SIZE_8, QSPI_CFG_BUS_SINGLE, 0);
    if (QSPI_STATUS_OK != status) {
        tr_error("QSPI Read failed");
        return status;
    }

    return QSPI_STATUS_OK;
}

qspi_status_t QSPIFBlockDevice::_qspi_send_program_command(qspi_inst_t progInst, const void *buffer, bd_addr_t addr,
                                                           bd_size_t *size)
{

    qspi_status_t status = _qspi_update_4byte_ext_addr_reg(addr);
    if (QSPI_STATUS_OK != status) {
        tr_error("QSPI Write - Updating 4-byte addressing extended address register failed");
        return status;
    }

    // Send program (write) command to device driver
    status = _qspi.write(prog_inst, -1, addr, (char *)buffer, (size_t *)size);
    if (QSPI_STATUS_OK != status) {
        tr_error("QSPI Write failed");
        return status;
    }

    return QSPI_STATUS_OK;
}

qspi_status_t QSPIFBlockDevice::_qspi_send_erase_command(qspi_inst_t erase_inst, bd_addr_t addr, bd_size_t size)
{
    tr_debug("Inst: 0x%xh, addr: %llu, size: %llu", erase_inst, addr, size);

    qspi_status_t status = _qspi_update_4byte_ext_addr_reg(addr);
    if (QSPI_STATUS_OK != status) {
        tr_error("QSPI Erase - Updating 4-byte addressing extended address register failed");
        return status;
    }

    // Send erase command to driver
    status = _qspi.command_transfer(erase_inst,
                                    (((int) addr) & 0x00FFF000), // Align addr to 4096
                                    NULL, 0, NULL, 0); // Do not transmit or receive

    if (QSPI_STATUS_OK != status) {
        tr_error("QSPI Erase failed");
        return status;
    }

    return QSPI_STATUS_OK;
}

qspi_status_t QSPIFBlockDevice::_qspi_send_general_command(qspi_inst_t instruction, bd_addr_t addr,
                                                           const char *tx_buffer,
                                                           size_t tx_length, const char *rx_buffer, size_t rx_length)
{
    qspi_status_t status = _qspi_update_4byte_ext_addr_reg(addr);
    if (QSPI_STATUS_OK != status) {
        tr_error("QSPI Generic command - Updating 4-byte addressing extended address register failed");
        return status;
    }

    // Send a general command instruction to driver
    status = _qspi.command_transfer(instruction, (int)addr, tx_buffer, tx_length, rx_buffer, rx_length);
    if (QSPI_STATUS_OK != status) {
        tr_error("Sending Generic command: %x", instruction);
        return status;
    }

    return QSPI_STATUS_OK;
}

qspi_status_t QSPIFBlockDevice::_qspi_send_read_sfdp_command(bd_addr_t addr, void *rx_buffer, bd_size_t rx_length)
{
    size_t rx_len = rx_length;

    // SFDP read instruction requires 1-1-1 bus mode with 8 dummy cycles and a 3-byte address
    _qspi.configure_format(QSPI_CFG_BUS_SINGLE, QSPI_CFG_BUS_SINGLE, QSPI_CFG_ADDR_SIZE_24, QSPI_CFG_BUS_SINGLE,
                           QSPI_CFG_ALT_SIZE_8, QSPI_CFG_BUS_SINGLE, QSPIF_RSFDP_DUMMY_CYCLES);
    qspi_status_t status = _qspi.read(QSPIF_INST_RSFDP, -1, (unsigned int) addr, (char*) rx_buffer, &rx_len);
    // All commands other than Read and RSFDP use default 1-1-1 bus mode (Program/Erase are constrained by flash memory performance more than bus performance)
    _qspi.configure_format(QSPI_CFG_BUS_SINGLE, QSPI_CFG_BUS_SINGLE, _address_size, QSPI_CFG_BUS_SINGLE,
                           QSPI_CFG_ALT_SIZE_8, QSPI_CFG_BUS_SINGLE, 0);
    if (QSPI_STATUS_OK != status) {
        tr_error("Sending SFDP read instruction");
        return status;
    }

    return QSPI_STATUS_OK;
}

qspi_status_t QSPIFBlockDevice::_qspi_read_status_registers(uint8_t *reg_buffer)
{
    // Read Status Register 1
    qspi_status_t status = _qspi_send_general_command(QSPIF_INST_RSR1, QSPI_NO_ADDRESS_COMMAND,
                                        NULL, 0,
                                        (char *) &reg_buffer[0], 1);
    if (QSPI_STATUS_OK == status) {
        tr_debug("Reading Status Register 1 Success: value = 0x%x", (int) reg_buffer[0]);
    } else {
        tr_error("Reading Status Register 1 failed");
        return status;
    }

    // Read Status Register 2
    status = _qspi_send_general_command(_read_status_reg_2_inst, QSPI_NO_ADDRESS_COMMAND,
                                        NULL, 0,
                                        (char *) &reg_buffer[1], 1);
    if (QSPI_STATUS_OK == status) {
        tr_debug("Reading Status Register 2 Success: value = 0x%x", (int) reg_buffer[1]);
    } else {
        tr_error("Reading Status Register 2 failed");
        return status;
    }

    return QSPI_STATUS_OK;
}

qspi_status_t QSPIFBlockDevice::_qspi_write_status_registers(uint8_t *reg_buffer)
{
    qspi_status_t status;

    if (_write_status_reg_2_inst == QSPI_NO_INST) {
        // Status registers are written on different data bytes of the same command
        if (_set_write_enable() != 0) {
            tr_error("Write Enable failed");
            return QSPI_STATUS_ERROR;
        }
        status = _qspi_send_general_command(QSPIF_INST_WSR1, QSPI_NO_ADDRESS_COMMAND,
                                            (char *) reg_buffer, 2,
                                            NULL, 0);
        if (QSPI_STATUS_OK == status) {
            tr_debug("Writing Status Registers Success: reg 1 value = 0x%x, reg 2 value = 0x%x",
                     (int) reg_buffer[0], (int) reg_buffer[1]);
        } else {
            tr_error("Writing Status Registers failed");
            return status;
        }
    } else {
        // Status registers are written using different commands

        // Write status register 1
        if (_set_write_enable() != 0) {
            tr_error("Write Enable failed");
            return QSPI_STATUS_ERROR;
        }
        status = _qspi_send_general_command(QSPIF_INST_WSR1, QSPI_NO_ADDRESS_COMMAND,
                                            (char *) &reg_buffer[0], 1,
                                            NULL, 0);
        if (QSPI_STATUS_OK == status) {
            tr_debug("Writing Status Register 1 Success: value = 0x%x",
                     (int) reg_buffer[0]);
        } else {
            tr_error("Writing Status Register 1 failed");
            return status;
        }

        // Write status register 2
        if (_set_write_enable() != 0) {
            tr_error("Write Enable failed");
            return QSPI_STATUS_ERROR;
        }
        status = _qspi_send_general_command(_write_status_reg_2_inst, QSPI_NO_ADDRESS_COMMAND,
                                            (char *) &reg_buffer[0], 1,
                                            NULL, 0);
        if (QSPI_STATUS_OK == status) {
            tr_debug("Writing Status Register 2 Success: value = 0x%x",
                     (int) reg_buffer[1]);
        } else {
            tr_error("Writing Status Register 2 failed");
            return status;
        }
    }

    return QSPI_STATUS_OK;
}
 
/*********************************************/
/************** Local Functions **************/
/*********************************************/
static int local_math_power(int base, int exp)
{
    // Integer X^Y function, used to calculate size fields given in 2^N format
    int result = 1;
    while (exp) {
        result *= base;
        exp--;
    }
    return result;
}
