/*
 * windows backend for libusb 1.0
 * Copyright © 2009-2012 Pete Batard <pete@akeo.ie>
 * With contributions from Michael Plante, Orin Eman et al.
 * Parts of this code adapted from libusb-win32-v1 by Stephan Meyer
 * HID Reports IOCTLs inspired from HIDAPI by Alan Ott, Signal 11 Software
 * Hash table functions adapted from glibc, by Ulrich Drepper et al.
 * Major code testing contribution by Xiaofan Chen
 * Hotplug support by Manuel Naranjo <naranjo.manuel@gmail.com> and
 * Chris Dickens <christopher.a.dickens@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include <windows.h>
#include <setupapi.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <process.h>
#include <stdio.h>
#include <inttypes.h>
#include <objbase.h>
#include <winioctl.h>

#include "libusbi.h"
#include "poll_windows.h"
#include "windows_usb.h"

// The 2 macros below are used in conjunction with safe loops.
#define LOOP_CHECK(fcall) { r=fcall; if (r != LIBUSB_SUCCESS) continue; }
#define LOOP_BREAK(err) { r=err; continue; }

// Helper prototypes
static int windows_get_active_config_descriptor(struct libusb_device *dev, unsigned char *buffer, size_t len, int *host_endian);
static int windows_clock_gettime(int clk_id, struct timespec *tp);
unsigned __stdcall windows_clock_gettime_threaded(void *param);
// Common calls

static int common_configure_endpoints(int sub_api, struct libusb_device_handle *dev_handle, int iface);
static void get_api_type(struct libusb_context *ctx, HDEVINFO *dev_info, SP_DEVINFO_DATA *dev_info_data, int *api, int *sub_api);
static int enumerate_device_interfaces(struct libusb_device *dev);
static int set_composite_interface(struct libusb_device *dev, char *dev_interface_path, char *device_id, int api, int sub_api);
static int set_hid_interface(struct libusb_device *dev, char *dev_interface_path);
static void unset_composite_interface(struct libusb_device *dev, char *device_id);
static void unset_hid_interface(struct libusb_device *dev, char *dev_interface_path);
static int init_device(struct libusb_device *dev, struct libusb_device *parent_dev, uint8_t port_number);

// WinUSB-like API prototypes
static int winusbx_init(int sub_api, struct libusb_context *ctx);
static int winusbx_exit(int sub_api);
static int winusbx_open(int sub_api, struct libusb_device_handle *dev_handle);
static void winusbx_close(int sub_api, struct libusb_device_handle *dev_handle);
static int winusbx_configure_endpoints(int sub_api, struct libusb_device_handle *dev_handle, int iface);
static int winusbx_claim_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface);
static int winusbx_release_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface);
static int winusbx_submit_control_transfer(int sub_api, struct usbi_transfer *itransfer);
static int winusbx_set_interface_altsetting(int sub_api, struct libusb_device_handle *dev_handle, int iface, int altsetting);
static int winusbx_submit_bulk_transfer(int sub_api, struct usbi_transfer *itransfer);
static int winusbx_clear_halt(int sub_api, struct libusb_device_handle *dev_handle, unsigned char endpoint);
static int winusbx_abort_transfers(int sub_api, struct usbi_transfer *itransfer);
static int winusbx_abort_control(int sub_api, struct usbi_transfer *itransfer);
static int winusbx_reset_device(int sub_api, struct libusb_device_handle *dev_handle);
static int winusbx_copy_transfer_data(int sub_api, struct usbi_transfer *itransfer, uint32_t io_size);
// HID API prototypes
static int hid_init(int sub_api, struct libusb_context *ctx);
static int hid_exit(int sub_api);
static int hid_open(int sub_api, struct libusb_device_handle *dev_handle);
static void hid_close(int sub_api, struct libusb_device_handle *dev_handle);
static int hid_claim_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface);
static int hid_release_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface);
static int hid_set_interface_altsetting(int sub_api, struct libusb_device_handle *dev_handle, int iface, int altsetting);
static int hid_submit_control_transfer(int sub_api, struct usbi_transfer *itransfer);
static int hid_submit_bulk_transfer(int sub_api, struct usbi_transfer *itransfer);
static int hid_clear_halt(int sub_api, struct libusb_device_handle *dev_handle, unsigned char endpoint);
static int hid_abort_transfers(int sub_api, struct usbi_transfer *itransfer);
static int hid_reset_device(int sub_api, struct libusb_device_handle *dev_handle);
static int hid_copy_transfer_data(int sub_api, struct usbi_transfer *itransfer, uint32_t io_size);
// Composite API prototypes
static int composite_init(int sub_api, struct libusb_context *ctx);
static int composite_exit(int sub_api);
static int composite_open(int sub_api, struct libusb_device_handle *dev_handle);
static void composite_close(int sub_api, struct libusb_device_handle *dev_handle);
static int composite_claim_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface);
static int composite_set_interface_altsetting(int sub_api, struct libusb_device_handle *dev_handle, int iface, int altsetting);
static int composite_release_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface);
static int composite_submit_control_transfer(int sub_api, struct usbi_transfer *itransfer);
static int composite_submit_bulk_transfer(int sub_api, struct usbi_transfer *itransfer);
static int composite_submit_iso_transfer(int sub_api, struct usbi_transfer *itransfer);
static int composite_clear_halt(int sub_api, struct libusb_device_handle *dev_handle, unsigned char endpoint);
static int composite_abort_transfers(int sub_api, struct usbi_transfer *itransfer);
static int composite_abort_control(int sub_api, struct usbi_transfer *itransfer);
static int composite_reset_device(int sub_api, struct libusb_device_handle *dev_handle);
static int composite_copy_transfer_data(int sub_api, struct usbi_transfer *itransfer, uint32_t io_size);


// Global variables
uint64_t hires_frequency, hires_ticks_to_ps;
const uint64_t epoch_time = UINT64_C(116444736000000000);	// 1970.01.01 00:00:000 in MS Filetime
int windows_version = WINDOWS_UNDEFINED;
static char windows_version_str[128] = "Windows Undefined";
char *host_controller[MAX_USB_HOST_CONTROLLERS] = { NULL };
usbi_mutex_t host_controller_lock;

// Concurrency
static int concurrent_usage = -1;
usbi_mutex_t autoclaim_lock;
// Timer thread
// NB: index 0 is for monotonic and 1 is for the thread exit event
HANDLE timer_thread = NULL;
HANDLE timer_mutex = NULL;
struct timespec timer_tp;
volatile LONG request_count[2] = {0, 1};	// last one must be > 0
HANDLE timer_request[2] = { NULL, NULL };
HANDLE timer_response = NULL;

// Hotplug thread
HANDLE hotplug_thread = NULL;
HANDLE hotplug_response = NULL;
BOOL hotplug_ready = FALSE;
HWND hHotplugMessage = NULL;

// API globals
#define CHECK_WINUSBX_AVAILABLE(sub_api) do { if (sub_api == SUB_API_NOTSET) sub_api = priv->sub_api; \
	if (!WinUSBX[sub_api].initialized) return LIBUSB_ERROR_ACCESS; } while(0)

static struct winusb_interface WinUSBX[SUB_API_MAX];
const char *sub_api_name[SUB_API_MAX] = WINUSBX_DRV_NAMES;
bool api_hid_available = false;
#define CHECK_HID_AVAILABLE do { if (!api_hid_available) return LIBUSB_ERROR_ACCESS; } while (0)
GUID HID_guid = { 0 };

static inline bool guid_eq(const GUID *guid1, const GUID *guid2) {
	if ((guid1 != NULL) && (guid2 != NULL)) {
		if (guid1 == guid2) {
			return true;
		}
		else {
			return (memcmp(guid1, guid2, sizeof(GUID)) == 0);
		}
	}
	return false;
}

#if defined(ENABLE_LOGGING)
static char *guid_to_string(const GUID *guid)
{
	static char guid_string[MAX_GUID_STRING_LENGTH];

	if (guid == NULL) return NULL;
	sprintf(guid_string, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		(unsigned int)guid->Data1, guid->Data2, guid->Data3,
		guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
		guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
	return guid_string;
}
#endif

/*
 * Converts a windows error to human readable string
 * uses retval as errorcode, or, if 0, use GetLastError()
 */
#if defined(ENABLE_LOGGING)
static char *windows_error_str(uint32_t retval)
{
static char err_string[ERR_BUFFER_SIZE];

	DWORD size;
	ssize_t i;
	uint32_t error_code, format_error;

	error_code = retval?retval:GetLastError();

	safe_sprintf(err_string, ERR_BUFFER_SIZE, "[%u] ", error_code);

	// Translate codes returned by SetupAPI. The ones we are dealing with are either
	// in 0x0000xxxx or 0xE000xxxx and can be distinguished from standard error codes.
	// See http://msdn.microsoft.com/en-us/library/windows/hardware/ff545011.aspx
	switch (error_code & 0xE0000000) {
	case 0:
		error_code = HRESULT_FROM_WIN32(error_code);	// Still leaves ERROR_SUCCESS unmodified
		break;
	case 0xE0000000:
		error_code =  0x80000000 | (FACILITY_SETUPAPI << 16) | (error_code & 0x0000FFFF);
		break;
	default:
		break;
	}

	size = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, error_code,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), &err_string[safe_strlen(err_string)],
		ERR_BUFFER_SIZE - (DWORD)safe_strlen(err_string), NULL);
	if (size == 0) {
		format_error = GetLastError();
		if (format_error)
			safe_sprintf(err_string, ERR_BUFFER_SIZE,
				"Windows error code %u (FormatMessage error code %u)", error_code, format_error);
		else
			safe_sprintf(err_string, ERR_BUFFER_SIZE, "Unknown error code %u", error_code);
	} else {
		// Remove CR/LF terminators
		for (i=safe_strlen(err_string)-1; (i>=0) && ((err_string[i]==0x0A) || (err_string[i]==0x0D)); i--) {
			err_string[i] = 0;
		}
	}
	return err_string;
}
#endif

static inline void windows_device_priv_init(libusb_device *dev) {
	struct windows_device_priv *p = _device_priv(dev);
	int i;
	p->depth = 0;
	p->port = 0;
	p->device_id = NULL;
	p->path = NULL;
	p->apib = &usb_api_backend[USB_API_UNSUPPORTED];
	p->sub_api = SUB_API_NOTSET;
	p->hid = NULL;
	p->active_config = 0;
	p->config_descriptor = NULL;
	memset(&(p->dev_descriptor), 0, sizeof(USB_DEVICE_DESCRIPTOR));
	for (i = 0; i<USB_MAXINTERFACES; i++) {
		p->usb_interface[i].path = NULL;
		p->usb_interface[i].apib = &usb_api_backend[USB_API_UNSUPPORTED];
		p->usb_interface[i].sub_api = SUB_API_NOTSET;
		p->usb_interface[i].nb_endpoints = 0;
		p->usb_interface[i].endpoint = NULL;
		p->usb_interface[i].restricted_functionality = false;
	}
}

static inline void windows_device_priv_release(libusb_device *dev) {
	struct windows_device_priv *p = _device_priv(dev);
	int i;
	safe_free(p->device_id);
	safe_free(p->path);
	if ((dev->num_configurations > 0) && (p->config_descriptor != NULL)) {
		for (i = 0; i < dev->num_configurations; i++)
			safe_free(p->config_descriptor[i]);
	}
	safe_free(p->config_descriptor);
	safe_free(p->hid);
	for (i = 0; i<USB_MAXINTERFACES; i++) {
		safe_free(p->usb_interface[i].path);
		safe_free(p->usb_interface[i].endpoint);
	}
}

/*
 * Sanitize Microsoft's paths: convert to uppercase, add prefix and fix backslashes.
 * Return an allocated sanitized string or NULL on error.
 */
static char *sanitize_path(const char *path)
{
	const char root_prefix[] = "\\\\.\\";
	size_t j, size, root_size;
	char *ret_path = NULL;
	size_t add_root = 0;

	if (path == NULL)
		return NULL;

	size = safe_strlen(path)+1;
	root_size = sizeof(root_prefix)-1;

	// Microsoft indiscriminately uses '\\?\', '\\.\', '##?#" or "##.#" for root prefixes.
	if (!((size > 3) && (((path[0] == '\\') && (path[1] == '\\') && (path[3] == '\\')) ||
		((path[0] == '#') && (path[1] == '#') && (path[3] == '#'))))) {
		add_root = root_size;
		size += add_root;
	}

	if ((ret_path = (char*) calloc(size, 1)) == NULL)
		return NULL;

	safe_strcpy(&ret_path[add_root], size-add_root, path);

	// Ensure consistency with root prefix
	for (j=0; j<root_size; j++)
		ret_path[j] = root_prefix[j];

	// Same goes for '\' and '#' after the root prefix. Ensure '#' is used
	for(j=root_size; j<size; j++) {
		ret_path[j] = (char)toupper((int)ret_path[j]);	// Fix case too
		if (ret_path[j] == '\\')
			ret_path[j] = '#';
	}

	return ret_path;
}

/*
 * Cfgmgr32, User32, OLE32 and SetupAPI DLL functions
 */
static int init_dlls(void)
{
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Parent, TRUE);
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Child, TRUE);
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Sibling, TRUE);
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Device_IDA, TRUE);
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Device_ID_Size, TRUE);

	// Prefixed to avoid conflict with header files
	DLL_LOAD_PREFIXED(OLE32.dll, p, CLSIDFromString, TRUE);
	DLL_LOAD_PREFIXED(SetupAPI.dll, p, SetupDiGetClassDevsA, TRUE);
	DLL_LOAD_PREFIXED(SetupAPI.dll, p, SetupDiGetClassDevsExA, TRUE);
	DLL_LOAD_PREFIXED(SetupAPI.dll, p, SetupDiEnumDeviceInfo, TRUE);
	DLL_LOAD_PREFIXED(SetupAPI.dll, p, SetupDiEnumDeviceInterfaces, TRUE);
	DLL_LOAD_PREFIXED(SetupAPI.dll, p, SetupDiGetDeviceInterfaceDetailA, TRUE);
	DLL_LOAD_PREFIXED(SetupAPI.dll, p, SetupDiDestroyDeviceInfoList, TRUE);
	DLL_LOAD_PREFIXED(SetupAPI.dll, p, SetupDiOpenDevRegKey, TRUE);
	DLL_LOAD_PREFIXED(SetupAPI.dll, p, SetupDiGetDeviceRegistryPropertyA, TRUE);
	DLL_LOAD_PREFIXED(SetupAPI.dll, p, SetupDiOpenDeviceInterfaceRegKey, TRUE);
	DLL_LOAD_PREFIXED(AdvAPI32.dll, p, RegQueryValueExW, TRUE);
	DLL_LOAD_PREFIXED(AdvAPI32.dll, p, RegCloseKey, TRUE);
	DLL_LOAD_PREFIXED(User32.dll, p, RegisterClassExA, TRUE);
	DLL_LOAD_PREFIXED(User32.dll, p, RegisterDeviceNotificationA, TRUE);
	DLL_LOAD_PREFIXED(User32.dll, p, UnregisterDeviceNotification, TRUE);
	DLL_LOAD_PREFIXED(User32.dll, p, UnregisterClassA, TRUE);

	return LIBUSB_SUCCESS;
}


/*
 * Get dev_info data from a device interface GUID
 *
 * Parameters:
 * dev_info: a pointer to a dev_info list
 * dev_info_data: a pointer to an SP_DEVINFO_DATA structure to be filled
 * guid: the GUID for which to retrieve interface details
 *
 * Note: It is the responsibility of the caller to call this function repeatedly
 * using the same guid (with an incremented index starting at zero) until all
 * interfaces have been traversed. This function is useful when it is not necessary to
 * obtain the SP_DEVICE_INTERFACE_DETAIL_DATA.
 */
static bool get_dev_info_data_by_guid(struct libusb_context *ctx, HDEVINFO *dev_info,
	SP_DEVINFO_DATA *dev_info_data, const GUID *guid, unsigned _index)
{
	if (_index == 0) {
		*dev_info = pSetupDiGetClassDevsA(guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	}

	if (*dev_info == INVALID_HANDLE_VALUE) {
		return false;
	}

	dev_info_data->cbSize = sizeof(SP_DEVINFO_DATA);
	if (!pSetupDiEnumDeviceInfo(*dev_info, _index, dev_info_data)) {
		if (GetLastError() != ERROR_NO_MORE_ITEMS) {
			usbi_err(ctx, "could not obtain device info data for index %u: %s",
				_index, windows_error_str(0));
		}
		pSetupDiDestroyDeviceInfoList(*dev_info);
		*dev_info = INVALID_HANDLE_VALUE;
		return false;
	}

	return true;
}

/*
 * Get dev_info data from a device instance ID
 *
 * Parameters:
 * dev_info: a pointer to a dev_info set
 * dev_info_data: a pointer to a DEVINFO_DATA structure to be filled
 * device_id: the device instance ID for which to retrieve the dev_info set
 * present: flag to only include present devices
 *
 * Note: On success, it is the responsibility of the caller to destroy
 * the dev_info set created by this function.
 */
static bool get_dev_info_data_by_device_id(struct libusb_context *ctx, HDEVINFO *dev_info,
	SP_DEVINFO_DATA *dev_info_data, const char *device_id, bool present)
{
	DWORD flags = DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE;
	SP_DEVINFO_DATA dummy;

	if (present) {
		flags |= DIGCF_PRESENT;
	}

	*dev_info = pSetupDiGetClassDevsA(NULL, device_id, NULL, flags);

	if (*dev_info == INVALID_HANDLE_VALUE) {
		return false;
	}

	// Since a device instance ID is used as the filter,
	// there should be at most one device in the dev_info set
	dev_info_data->cbSize = sizeof(SP_DEVINFO_DATA);
	if (!pSetupDiEnumDeviceInfo(*dev_info, 0, dev_info_data)) {
		goto err_exit;
	}

	// This checks that there is truly only one device
	dummy.cbSize = sizeof(SP_DEVINFO_DATA);
	if (pSetupDiEnumDeviceInfo(*dev_info, 1, &dummy)) {
		usbi_err(ctx, "program assertion failed: dev_info set has more than one item");
		goto err_exit;
	}

	return true;

err_exit:
	pSetupDiDestroyDeviceInfoList(*dev_info);
	*dev_info = INVALID_HANDLE_VALUE;
	return false;
}

/*
 * Retrieve the device interface details of a device interface
 *
 * Parameters:
 * dev_info: a pointer to a populated dev_info list
 * dev_interface_data: a pointer to an SP_DEVICE_INTERFACE_DATA structure (or NULL if not needed)
 * guid: the GUID for which to retrieve interface details
 *
 * Note: It is the responsibility of the caller to free the DEVICE_INTERFACE_DETAIL_DATA
 * structure returned. This function should be called using the results of get_dev_info_by_guid()
 * or get_dev_info_by_device_id() with an appropriate index.
 */
static SP_DEVICE_INTERFACE_DETAIL_DATA_A *get_interface_detail_actual(struct libusb_context *ctx,
	HDEVINFO *dev_info, SP_DEVICE_INTERFACE_DATA *dev_interface_data, const GUID *guid, unsigned _index)
{
	SP_DEVICE_INTERFACE_DATA local_dev_interface_data;
	SP_DEVICE_INTERFACE_DETAIL_DATA_A *dev_interface_details = NULL;
	DWORD size;

	// The setup functions below require a SP_DEVICE_INTERFACE_DATA structure,
	// but the caller might not care about that data. Use a structure on the
	// stack if the caller did not provide.
	if (dev_interface_data == NULL) {
		dev_interface_data = &local_dev_interface_data;
	}

	dev_interface_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
	if (!pSetupDiEnumDeviceInterfaces(*dev_info, NULL, guid, _index, dev_interface_data)) {
		if (GetLastError() != ERROR_NO_MORE_ITEMS) {
			usbi_err(ctx, "could not obtain interface data for index %u: %s",
				_index, windows_error_str(0));
		}
		return NULL;
	}

	// Read interface data (dummy + actual) to access the device path
	if (!pSetupDiGetDeviceInterfaceDetailA(*dev_info, dev_interface_data, NULL, 0, &size, NULL)) {
		// The dummy call should fail with ERROR_INSUFFICIENT_BUFFER
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			usbi_err(ctx, "could not access interface data (dummy) for index %u: %s",
				_index, windows_error_str(0));
			return NULL;
		}
	}
	else {
		usbi_err(ctx, "program assertion failed: http://msdn.microsoft.com/en-us/library/ms792901.aspx is wrong");
		return NULL;
	}

	dev_interface_details = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)calloc(1, size);
	if (dev_interface_details == NULL) {
		usbi_err(ctx, "could not allocate interface data for index %u", _index);
		return NULL;
	}

	dev_interface_details->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
	if (!pSetupDiGetDeviceInterfaceDetailA(*dev_info, dev_interface_data,
		dev_interface_details, size, &size, NULL)) {
		usbi_err(ctx, "could not access interface data (actual) for index %u: %s",
			_index, windows_error_str(0));
		free(dev_interface_details);
		return NULL;
	}

	return dev_interface_details;
}

/*
 * Enumerate interfaces for a specific GUID
 *
 * Parameters:
 * dev_info: a pointer to a DEVINFO set
 * dev_info_data: a pointer to an SP_DEVINFO_DATA structure to be filled
 * dev_interface_data: a pointer to an SP_DEVICE_INTERFACE_DATA to be filled (or NULL if not needed)
 * guid: the GUID for which to retrieve interface details
 *
 * Note: It is the responsibility of the caller to free the SP_DEVICE_INTERFACE_DETAIL_DATA
 * structure returned and call this function repeatedly using the same guid (with an
 * incremented index starting at zero) until all interfaces have been returned.
 */
static SP_DEVICE_INTERFACE_DETAIL_DATA_A *get_interface_detail(struct libusb_context *ctx,
	HDEVINFO *dev_info, SP_DEVINFO_DATA *dev_info_data, SP_DEVICE_INTERFACE_DATA *dev_interface_data,
	const GUID *guid, unsigned _index)
{
	SP_DEVICE_INTERFACE_DETAIL_DATA_A *dev_interface_details;

	if (!get_dev_info_data_by_guid(ctx, dev_info, dev_info_data, guid, _index)) {
		return NULL;
	}

	dev_interface_details = get_interface_detail_actual(ctx, dev_info, dev_interface_data, guid, _index);
	if (dev_interface_details == NULL) {
		pSetupDiDestroyDeviceInfoList(*dev_info);
		*dev_info = INVALID_HANDLE_VALUE;
		return NULL;
	}

	return dev_interface_details;
}

/* For libusb0 filter */
static SP_DEVICE_INTERFACE_DETAIL_DATA_A *get_interface_details_filter(struct libusb_context *ctx,
	HDEVINFO *dev_info, SP_DEVINFO_DATA *dev_info_data, const GUID *guid, unsigned _index, char *filter_path)
{
	SP_DEVICE_INTERFACE_DATA dev_interface_data;
	SP_DEVICE_INTERFACE_DETAIL_DATA_A *dev_interface_detail;

	dev_interface_detail = get_interface_detail(ctx, dev_info, dev_info_data, &dev_interface_data, guid, _index);

	// [trobinso] lookup the libusb0 symbolic index.
	if (dev_interface_detail != NULL) {
		HKEY hkey_device_interface = pSetupDiOpenDeviceInterfaceRegKey(*dev_info, &dev_interface_data, 0, KEY_READ);
		if (hkey_device_interface != INVALID_HANDLE_VALUE) {
			DWORD libusb0_symboliclink_index = 0;
			DWORD value_length = sizeof(DWORD);
			DWORD value_type = 0;
			LONG status;
			status = pRegQueryValueExW(hkey_device_interface, L"LUsb0", NULL, &value_type,
				(LPBYTE) &libusb0_symboliclink_index, &value_length);
			if (status == ERROR_SUCCESS) {
				if (libusb0_symboliclink_index < 256) {
					// libusb0.sys is connected to this device instance.
					// If the the device interface guid is {F9F3FF14-AE21-48A0-8A25-8011A7A931D9} then it's a filter.
					safe_sprintf(filter_path, sizeof("\\\\.\\libusb0-0000"), "\\\\.\\libusb0-%04d", libusb0_symboliclink_index);
					usbi_dbg("assigned libusb0 symbolic link %s", filter_path);
				} else {
					// libusb0.sys was connected to this device instance at one time; but not anymore.
				}
			}
			pRegCloseKey(hkey_device_interface);
		}
	}

	return dev_interface_detail;
}

/*
 * Extracts the device instance ID from a device interface path
 * Note: It is the responsibility of the caller to free the returned string.
 */
static char *parse_device_interface_path(const char *interface_path)
{
	char *device_id, *guid_start;
	unsigned int i, len;

	len = safe_strlen(interface_path);
	if (len < 4) {
		return NULL;
	}

	// Microsoft indiscriminatly uses '\\?\', '\\.\', '##?#" or "##.#" for root prefixes.
	if (((interface_path[0] == '\\') && (interface_path[1] == '\\') && (interface_path[3] == '\\')) ||
		((interface_path[0] == '#') && (interface_path[1] == '#') && (interface_path[3] == '#'))) {
		interface_path += 4;
		len -= 4;
	}

	guid_start = strchr(interface_path, '{');
	if (guid_start != NULL) {
		len = (guid_start - interface_path) - 1; // One more for separator
	}

	if (len <= 0) {
		usbi_err(NULL, "program assertion failed: invalid device interface path");
		return NULL;
	}

	device_id = calloc(len + 1, 1); // One additional for NULL term.
	if (device_id == NULL) {
		return NULL;
	}
	strncat(device_id, interface_path, len);

	// Now convert to uppercase and replace '#' with '\'
	for (i = 0; i < len; i++) {
		device_id[i] = (char)toupper((int)device_id[i]);
		if (device_id[i] == '#')
			device_id[i] = '\\';
	}

	return device_id;
}

/* Hash table functions - modified From glibc 2.3.2:
   [Aho,Sethi,Ullman] Compilers: Principles, Techniques and Tools, 1986
   [Knuth]            The Art of Computer Programming, part 3 (6.4)  */
typedef struct htab_entry {
	unsigned long used;
	char *str;
} htab_entry;
htab_entry *htab_table = NULL;
usbi_mutex_t htab_write_mutex = NULL;
unsigned long htab_size, htab_filled;

/* For the used double hash method the table size has to be a prime. To
   correct the user given table size we need a prime test.  This trivial
   algorithm is adequate because the code is called only during init and
   the number is likely to be small  */
static int isprime(unsigned long number)
{
	// no even number will be passed
	unsigned int divider = 3;

	while((divider * divider < number) && (number % divider != 0))
		divider += 2;

	return (number % divider != 0);
}

/* Before using the hash table we must allocate memory for it.
   We allocate one element more as the found prime number says.
   This is done for more effective indexing as explained in the
   comment for the hash function.  */
static int htab_create(struct libusb_context *ctx, unsigned long nel)
{
	if (htab_table != NULL) {
		usbi_err(ctx, "hash table already allocated");
		return 0;
	}

	// Create a mutex
	usbi_mutex_init(&htab_write_mutex, NULL);

	// Change nel to the first prime number not smaller as nel.
	nel |= 1;
	while(!isprime(nel))
		nel += 2;

	htab_size = nel;
	usbi_dbg("using %d entries hash table", nel);
	htab_filled = 0;

	// allocate memory and zero out.
	htab_table = (htab_entry*) calloc(htab_size + 1, sizeof(htab_entry));
	if (htab_table == NULL) {
		usbi_err(ctx, "could not allocate space for hash table");
		return 0;
	}

	return 1;
}

/* After using the hash table it has to be destroyed.  */
static void htab_destroy(void)
{
	size_t i;
	if (htab_table == NULL) {
		return;
	}

	for (i=0; i<htab_size; i++) {
		if (htab_table[i].used && htab_table[i].str != NULL) {
			safe_free(htab_table[i].str);
		}
	}

	usbi_mutex_destroy(&htab_write_mutex);

	safe_free(htab_table);
}

/* This is the search function. It uses double hashing with open addressing.
   We use an trick to speed up the lookup. The table is created with one
   more element available. This enables us to use the index zero special.
   This index will never be used because we store the first hash index in
   the field used where zero means not used. Every other value means used.
   The used field can be used as a first fast comparison for equality of
   the stored and the parameter value. This helps to prevent unnecessary
   expensive calls of strcmp.  */
static unsigned long htab_hash(const char *str)
{
	unsigned long hval, hval2;
	unsigned long idx;
	unsigned long r = 5381;
	int c;
	const char *sz = str;

	if (str == NULL)
		return 0;

	// Compute main hash value (algorithm suggested by Nokia)
	while ((c = *sz++) != 0)
		r = ((r << 5) + r) + c;
	if (r == 0)
		++r;

	// compute table hash: simply take the modulus
	hval = r % htab_size;
	if (hval == 0)
		++hval;

	// Try the first index
	idx = hval;

	if (htab_table[idx].used) {
		if ( (htab_table[idx].used == hval)
		  && (safe_strcmp(str, htab_table[idx].str) == 0) ) {
			// existing hash
			return idx;
		}
		usbi_dbg("hash collision ('%s' vs '%s')", str, htab_table[idx].str);

		// Second hash function, as suggested in [Knuth]
		hval2 = 1 + hval % (htab_size - 2);

		do {
			// Because size is prime this guarantees to step through all available indexes
			if (idx <= hval2) {
				idx = htab_size + idx - hval2;
			} else {
				idx -= hval2;
			}

			// If we visited all entries leave the loop unsuccessfully
			if (idx == hval) {
				break;
			}

			// If entry is found use it.
			if ( (htab_table[idx].used == hval)
			  && (safe_strcmp(str, htab_table[idx].str) == 0) ) {
				return idx;
			}
		}
		while (htab_table[idx].used);
	}

	// Not found => New entry

	// If the table is full return an error
	if (htab_filled >= htab_size) {
		usbi_err(NULL, "hash table is full (%d entries)", htab_size);
		return 0;
	}

	// Concurrent threads might be storing the same entry at the same time
	// (eg. "simultaneous" enums from different threads) => use a mutex
	usbi_mutex_lock(&htab_write_mutex);
	// Just free any previously allocated string (which should be the same as
	// new one). The possibility of concurrent threads storing a collision
	// string (same hash, different string) at the same time is extremely low
	safe_free(htab_table[idx].str);
	htab_table[idx].used = hval;
	htab_table[idx].str = (char*) calloc(safe_strlen(str)+1, sizeof(char));
	if (htab_table[idx].str == NULL) {
		htab_table[idx].used = 0;
		usbi_err(NULL, "could not duplicate string for hash table");
		usbi_mutex_unlock(&htab_write_mutex);
		return 0;
	}
	memcpy(htab_table[idx].str, str, safe_strlen(str));
	++htab_filled;
	usbi_mutex_unlock(&htab_write_mutex);

	return idx;
}

/*
 * Returns the device instance ID for the given device instance
 * If the device cannot be found, return NULL
 * Note: It is the responsibility of the caller to free the returned string
 */
static char *get_device_id(DWORD devinst)
{
	char *device_id = NULL;
	DWORD size;

	if (CM_Get_Device_ID_Size(&size, devinst, 0) != CR_SUCCESS) {
		usbi_dbg("could not retrieve device id size for device instance %d", devinst);
		return NULL;
	}

	device_id = calloc(++size, 1); // Add one for NULL term.
	if (device_id == NULL) {
		usbi_dbg("failed to allocate device id for device instance %d", devinst);
		return NULL;
	}

	if (CM_Get_Device_IDA(devinst, device_id, size, 0) != CR_SUCCESS) {
		usbi_dbg("could not retrieve device id for device instance %d", devinst);
		free(device_id);
		return NULL;
	}

	return device_id;
}

/*
 * Returns the device instance of a device's parent
 * If the parent cannot be found, return 0
 */
static DWORD get_parent_device_instance(DWORD devinst)
{
	DWORD parent_devinst;

	if (CM_Get_Parent(&parent_devinst, devinst, 0) != CR_SUCCESS) {
		return 0;
	}

	return parent_devinst;
}
/*
 * Returns the device instance ID of a device's parent
 * If the parent cannot be found, return NULL
 * Note: It is the responsibility of the caller to free the returned string.
 */
static char *get_parent_device_id(DWORD devinst)
{
	DWORD parent_devinst = get_parent_device_instance(devinst);

	if (parent_devinst == 0) {
		return NULL;
	}

	return get_device_id(parent_devinst);
}

/*
 * Populate the endpoints addresses of the device_priv interface helper structs
 */
static int windows_assign_endpoints(struct libusb_device_handle *dev_handle, int iface, int altsetting)
{
	int i, r;
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	struct libusb_config_descriptor *conf_desc;
	const struct libusb_interface_descriptor *if_desc;
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);

	r = libusb_get_config_descriptor(dev_handle->dev, 0, &conf_desc);
	if (r != LIBUSB_SUCCESS) {
		usbi_warn(ctx, "could not read config descriptor: error %d", r);
		return r;
	}

	if_desc = &conf_desc->interface[iface].altsetting[altsetting];
	safe_free(priv->usb_interface[iface].endpoint);

	if (if_desc->bNumEndpoints == 0) {
		usbi_dbg("no endpoints found for interface %d", iface);
		return LIBUSB_SUCCESS;
	}

	priv->usb_interface[iface].endpoint = (uint8_t*) malloc(if_desc->bNumEndpoints);
	if (priv->usb_interface[iface].endpoint == NULL) {
		return LIBUSB_ERROR_NO_MEM;
	}

	priv->usb_interface[iface].nb_endpoints = if_desc->bNumEndpoints;
	for (i=0; i<if_desc->bNumEndpoints; i++) {
		priv->usb_interface[iface].endpoint[i] = if_desc->endpoint[i].bEndpointAddress;
		usbi_dbg("(re)assigned endpoint %02X to interface %d", priv->usb_interface[iface].endpoint[i], iface);
	}
	libusb_free_config_descriptor(conf_desc);

	// Extra init may be required to configure endpoints
	return priv->apib->configure_endpoints(SUB_API_NOTSET, dev_handle, iface);
}

// Lookup for a match in the list of API driver names
// return -1 if not found, driver match number otherwise
static int get_sub_api(char *driver, int api){
	int i;
	const char sep_str[2] = {LIST_SEPARATOR, 0};
	char *tok, *tmp_str;
	size_t len = safe_strlen(driver);

	if (len == 0) return SUB_API_NOTSET;
	tmp_str = (char*) calloc(len+1, 1);
	if (tmp_str == NULL) return SUB_API_NOTSET;
	memcpy(tmp_str, driver, len+1);
	tok = strtok(tmp_str, sep_str);
	while (tok != NULL) {
		for (i=0; i<usb_api_backend[api].nb_driver_names; i++) {
			if (safe_stricmp(tok, usb_api_backend[api].driver_name_list[i]) == 0) {
				free(tmp_str);
				return i;
			}
		}
		tok = strtok(NULL, sep_str);
	}
	free (tmp_str);
	return SUB_API_NOTSET;
}

/*
 * auto-claiming and auto-release helper functions
 */
static int auto_claim(struct libusb_transfer *transfer, int *interface_number, int api_type)
{
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(
		transfer->dev_handle);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	int current_interface = *interface_number;
	int r = LIBUSB_SUCCESS;

	switch(api_type) {
	case USB_API_WINUSBX:
	case USB_API_HID:
		break;
	default:
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	usbi_mutex_lock(&autoclaim_lock);
	if (current_interface < 0)	// No serviceable interface was found
	{
		for (current_interface=0; current_interface<USB_MAXINTERFACES; current_interface++) {
			// Must claim an interface of the same API type
			if ( (priv->usb_interface[current_interface].apib->id == api_type)
			  && (libusb_claim_interface(transfer->dev_handle, current_interface) == LIBUSB_SUCCESS) ) {
				usbi_dbg("auto-claimed interface %d for control request", current_interface);
				if (handle_priv->autoclaim_count[current_interface] != 0) {
					usbi_warn(ctx, "program assertion failed - autoclaim_count was nonzero");
				}
				handle_priv->autoclaim_count[current_interface]++;
				break;
			}
		}
		if (current_interface == USB_MAXINTERFACES) {
			usbi_err(ctx, "could not auto-claim any interface");
			r = LIBUSB_ERROR_NOT_FOUND;
		}
	} else {
		// If we have a valid interface that was autoclaimed, we must increment
		// its autoclaim count so that we can prevent an early release.
		if (handle_priv->autoclaim_count[current_interface] != 0) {
			handle_priv->autoclaim_count[current_interface]++;
		}
	}
	usbi_mutex_unlock(&autoclaim_lock);

	*interface_number = current_interface;
	return r;

}

static void auto_release(struct usbi_transfer *itransfer)
{
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	libusb_device_handle *dev_handle = transfer->dev_handle;
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	int r;

	usbi_mutex_lock(&autoclaim_lock);
	if (handle_priv->autoclaim_count[transfer_priv->interface_number] > 0) {
		handle_priv->autoclaim_count[transfer_priv->interface_number]--;
		if (handle_priv->autoclaim_count[transfer_priv->interface_number] == 0) {
			r = libusb_release_interface(dev_handle, transfer_priv->interface_number);
			if (r == LIBUSB_SUCCESS) {
				usbi_dbg("auto-released interface %d", transfer_priv->interface_number);
			} else {
				usbi_dbg("failed to auto-release interface %d (%s)",
					transfer_priv->interface_number, libusb_error_name((enum libusb_error)r));
			}
		}
	}
	usbi_mutex_unlock(&autoclaim_lock);
}

/* Windows version dtection */
static BOOL is_x64(void)
{
	BOOL ret = FALSE;
	// Detect if we're running a 32 or 64 bit system
	if (sizeof(uintptr_t) < 8) {
		DLL_LOAD_PREFIXED(Kernel32.dll, p, IsWow64Process, FALSE);
		if (pIsWow64Process != NULL) {
			(*pIsWow64Process)(GetCurrentProcess(), &ret);
		}
	} else {
		ret = TRUE;
	}
	return ret;
}

static void get_windows_version(void)
{
	OSVERSIONINFOEXA vi, vi2;
	const char* w = 0;
	const char* w64 = "32 bit";
	char* vptr;
	size_t vlen;
	unsigned major, minor;
	ULONGLONG major_equal, minor_equal;
	BOOL ws;

	memset(&vi, 0, sizeof(vi));
	vi.dwOSVersionInfoSize = sizeof(vi);
	if (!GetVersionExA((OSVERSIONINFOA *)&vi)) {
		memset(&vi, 0, sizeof(vi));
		vi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
		if (!GetVersionExA((OSVERSIONINFOA *)&vi))
			return;
	}

	if (vi.dwPlatformId == VER_PLATFORM_WIN32_NT) {

		if (vi.dwMajorVersion > 6 || (vi.dwMajorVersion == 6 && vi.dwMinorVersion >= 2)) {
			// Starting with Windows 8.1 Preview, GetVersionEx() does no longer report the actual OS version
			// See: http://msdn.microsoft.com/en-us/library/windows/desktop/dn302074.aspx

			major_equal = VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL);
			for (major = vi.dwMajorVersion; major <= 9; major++) {
				memset(&vi2, 0, sizeof(vi2));
				vi2.dwOSVersionInfoSize = sizeof(vi2); vi2.dwMajorVersion = major;
				if (!VerifyVersionInfoA(&vi2, VER_MAJORVERSION, major_equal))
					continue;
				if (vi.dwMajorVersion < major) {
					vi.dwMajorVersion = major; vi.dwMinorVersion = 0;
				}

				minor_equal = VerSetConditionMask(0, VER_MINORVERSION, VER_EQUAL);
				for (minor = vi.dwMinorVersion; minor <= 9; minor++) {
					memset(&vi2, 0, sizeof(vi2)); vi2.dwOSVersionInfoSize = sizeof(vi2);
					vi2.dwMinorVersion = minor;
					if (!VerifyVersionInfoA(&vi2, VER_MINORVERSION, minor_equal))
						continue;
					vi.dwMinorVersion = minor;
					break;
				}

				break;
			}
		}

		if (vi.dwMajorVersion <= 0xf && vi.dwMinorVersion <= 0xf) {
			ws = (vi.wProductType <= VER_NT_WORKSTATION);
			windows_version = vi.dwMajorVersion << 4 | vi.dwMinorVersion;
			switch (windows_version) {
			case 0x50: w = "2000";
				break;
			case 0x51: w = "XP";
				break;
			case 0x52: w = ("2003");
				break;
			case 0x60: w = (ws?"Vista":"2008");
				break;
			case 0x61: w = (ws?"7":"2008_R2");
				break;
			case 0x62: w = (ws?"8":"2012");
				break;
			case 0x63: w = (ws?"8.1":"2012_R2");
				break;
			case 0x64: w = (ws?"8.2":"2012_R3");
				break;
			default:
				if (windows_version < 0x50)
					windows_version = WINDOWS_UNSUPPORTED;
				else
					w = "9 or later";
				break;
			}
		}
	}

	if (is_x64())
		w64 = "64-bit";

	vptr = &windows_version_str[sizeof("Windows ") - 1];
	vlen = sizeof(windows_version_str) - sizeof("Windows ") - 1;
	if (!w)
		safe_sprintf(vptr, vlen, "%s %u.%u %s", (vi.dwPlatformId==VER_PLATFORM_WIN32_NT?"NT":"??"),
			(unsigned)vi.dwMajorVersion, (unsigned)vi.dwMinorVersion, w64);
	else if (vi.wServicePackMinor)
		safe_sprintf(vptr, vlen, "%s SP%u.%u %s", w, vi.wServicePackMajor, vi.wServicePackMinor, w64);
	else if (vi.wServicePackMajor)
		safe_sprintf(vptr, vlen, "%s SP%u %s", w, vi.wServicePackMajor, w64);
	else
		safe_sprintf(vptr, vlen, "%s %s", w, w64);
}

/*
 * Retrieve the port number and installation state for a device
 */
static int get_device_port_and_state(struct libusb_context *ctx, HDEVINFO *dev_info,
	SP_DEVINFO_DATA *dev_info_data, const char *dev_id, DWORD *port_number)
{
	DWORD install_state, port, size;

	if (!pSetupDiGetDeviceRegistryPropertyA(*dev_info, dev_info_data,
		SPDRP_INSTALL_STATE, NULL, (BYTE *)&install_state, 4, &size) || (size != 4)) {
		usbi_warn(ctx, "could not detect installation state of driver for '%s': %s",
			dev_id, windows_error_str(0));
		return LIBUSB_ERROR_ACCESS;
	}
	else if (install_state != 0) {
		usbi_warn(ctx, "driver for device '%s' is reporting an issue (code: %d) - skipping",
			dev_id, install_state);
		return LIBUSB_ERROR_ACCESS;
	}

	// The SPDRP_ADDRESS for USB devices is the device port number on the hub
	if (!pSetupDiGetDeviceRegistryPropertyA(*dev_info, dev_info_data,
		SPDRP_ADDRESS, NULL, (BYTE *)&port, 4, &size) || (size != 4)) {
		usbi_warn(ctx, "could not retrieve port number for device '%s', skipping: %s",
			dev_id, windows_error_str(0));
		return LIBUSB_ERROR_ACCESS;
	}
	*port_number = port;

	return LIBUSB_SUCCESS;
}

/*
 * Handles complete enumeration of a USB device
 * 
 * Parameters:
 * device_id: the device instance ID string
 * guid: device interface GUID (either USB_HUB or USB_DEVICE)
 *
 * Note: If the device cannot be enumerated, it will not be
 * added to the device list for the given context.
 */
static void windows_enumerate_device(libusb_context *ctx, const char *device_id, const GUID *guid)
{
	HDEVINFO dev_info = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA dev_info_data = { 0 };
	SP_DEVICE_INTERFACE_DETAIL_DATA_A *dev_interface_detail = NULL;
	DWORD port_number;
	struct libusb_device *dev = NULL, *parent_dev = NULL;
	struct windows_device_priv *priv;
	const GUID *parent_guid;
	char *parent_device_id = NULL;
	int api, sub_api;
	unsigned int i;
	unsigned long session_id, parent_session_id;

	// If the device already exists in the session, there's nothing to do
	session_id = htab_hash(device_id);
	dev = usbi_get_device_by_session_id(ctx, session_id);
	if (dev != NULL) {
		usbi_dbg("device found in session [%X] (%d.%d)", session_id, dev->bus_number, dev->device_address);
		libusb_unref_device(dev);
		return;
	}

	if (!get_dev_info_data_by_device_id(ctx, &dev_info, &dev_info_data, device_id, TRUE)) {
		usbi_dbg("device '%s' not found", device_id);
		return;
	}

	if (get_device_port_and_state(ctx, &dev_info, &dev_info_data, device_id, &port_number) != LIBUSB_SUCCESS) {
		usbi_err(ctx, "device '%s' not in a good state", device_id);
		goto cleanup;
	}

	// If this is a HUB and the port number is zero,
	// then this is a root hub (no parent device needed)
	if (port_number == 0 && guid_eq(guid, &GUID_DEVINTERFACE_USB_HUB)) {
		goto alloc_device;
	}

	parent_device_id = get_parent_device_id(dev_info_data.DevInst);
	if (parent_device_id == NULL) {
		usbi_warn(ctx, "could not get parent device id for '%s'", device_id);
		goto cleanup;
	}

	parent_session_id = htab_hash(parent_device_id);
	parent_dev = usbi_get_device_by_session_id(ctx, parent_session_id);
	if (parent_dev == NULL) {
		usbi_dbg("parent for '%s' not found, enumerating now", device_id);
		if (guid_eq(guid,  &GUID_DEVINTERFACE_USB_DEVICE)) {
			usbi_dbg("'%s' GUID is a DEVICE, parent GUID should be HUB", device_id);
			parent_guid = &GUID_DEVINTERFACE_USB_HUB;
		}
		else if (guid_eq(guid, &GUID_DEVINTERFACE_USB_HUB)) {
			usbi_dbg("'%s' GUID is a HUB, parent GUID should be HUB", device_id);
			parent_guid = &GUID_DEVINTERFACE_USB_HUB;
		}
		else {
			usbi_err(ctx, "program assertion failed - unknown GUID");
			parent_guid = NULL;
		}
		windows_enumerate_device(ctx, parent_device_id, parent_guid);
		parent_dev = usbi_get_device_by_session_id(ctx, parent_session_id);
		if (parent_dev == NULL) {
			usbi_warn(ctx, "unable to enumerate parent '%s' for '%s'", parent_device_id, device_id);
			goto cleanup;
		}
	}

alloc_device:
	usbi_dbg("PRO: %s", device_id);
	usbi_dbg("allocating new device for session [%X]", session_id);

	get_api_type(ctx, &dev_info, &dev_info_data, &api, &sub_api);

	dev = usbi_alloc_device(ctx, session_id);
	if (dev == NULL) {
		usbi_warn(ctx, "failed to allocate new device for '%s'", device_id);
		goto cleanup;
	}

	windows_device_priv_init(dev);
	priv = _device_priv(dev);

	priv->devinst = dev_info_data.DevInst;

	// Index is 0 because there is only one device in the dev_info set
	dev_interface_detail = get_interface_detail_actual(ctx, &dev_info, NULL, guid, 0);
	if (dev_interface_detail == NULL) {
		usbi_warn(ctx, "could not get interface detail for '%s'", device_id);
		goto cleanup;
	}

	priv->device_id = _strdup(device_id);
	if (priv->device_id == NULL) {
		usbi_warn(ctx, "failed to allocate device id string for '%s'", device_id);
		goto cleanup;
	}

	priv->path = sanitize_path(dev_interface_detail->DevicePath);
	if (priv->path == NULL) {
		usbi_warn(ctx, "failed to allocate interface path for '%s'", device_id);
		goto cleanup;
	}

	priv->apib = &usb_api_backend[api];
	priv->sub_api = sub_api;

	switch (api) {
	case USB_API_COMPOSITE:
	case USB_API_HUB:
		break;
	case USB_API_HID:
		priv->hid = calloc(1, sizeof(struct hid_device_priv));
		if (priv->hid == NULL) {
			usbi_warn(ctx, "failed allocating hid for '%s'", device_id);
			goto cleanup;
		}
		priv->hid->nb_interfaces = 0;
		break;
	default:
		// For other devices, first interface is the same as the device
		priv->usb_interface[0].path = _strdup(priv->path);
		if (priv->usb_interface[0].path == NULL) {
			usbi_warn(ctx, "could not duplicate interface path for '%s'", device_id);
			goto cleanup;
		}

		// The following is needed if we want API calls to work for
		// both simple and composite devices.
		for (i = 0; i < USB_MAXINTERFACES; i++) {
			priv->usb_interface[i].apib = &usb_api_backend[api];
		}
		break;
	}

	if (init_device(dev, parent_dev, (uint8_t)port_number) != LIBUSB_SUCCESS) {
		usbi_warn(ctx, "failed to initialize device '%s'", device_id);
		goto cleanup;
	}

	switch (api) {
	case USB_API_COMPOSITE:
	case USB_API_HID:
		if (enumerate_device_interfaces(dev) != LIBUSB_SUCCESS) {
			usbi_warn(ctx, "failed to enumerate interfaces for '%s'", device_id);
			goto cleanup;
		}
		break;
	default:
		break;
	}

	usbi_connect_device(dev);
	dev = NULL;

cleanup:
	safe_free(parent_device_id);
	safe_free(dev_interface_detail);
	if (dev_info != INVALID_HANDLE_VALUE) {
		pSetupDiDestroyDeviceInfoList(dev_info);
	}
	if (parent_dev != NULL) {
		libusb_unref_device(parent_dev);
	}
	// This will destroy the newly allocated device
	if (dev != NULL) {
		libusb_unref_device(dev);
	}
}

/*
 * Enumerate a device for each context
 *
 * Parameters:
 * device_id: the device instance ID string
 * guid: device interface GUID (either USB_HUB or USB_DEVICE)
 */
static void windows_hotplug_enumerate(const char *device_id, CONST GUID *guid)
{
	struct libusb_context *ctx;

	usbi_mutex_static_lock(&active_contexts_lock);
	list_for_each_entry(ctx, &active_contexts_list, list, struct libusb_context) {
		windows_enumerate_device(ctx, device_id, guid);
	}
	usbi_mutex_static_unlock(&active_contexts_lock);
}

/*
 * Remove a device from each context
 *
 * Parameters:
 * device_id: the device instance ID string
 */
static void windows_hotplug_disconnect(const char *device_id)
{
	struct libusb_context *ctx;
	struct libusb_device *dev;
	unsigned long session_id;

 	session_id = htab_hash(device_id);

	usbi_mutex_static_lock(&active_contexts_lock);
	list_for_each_entry(ctx, &active_contexts_list, list, struct libusb_context) {
		dev = usbi_get_device_by_session_id(ctx, session_id);
		if (dev != NULL) {
			usbi_dbg("device disconnected [%X] (%d.%d)", session_id, dev->bus_number, dev->device_address);
			usbi_disconnect_device(dev);
			libusb_unref_device(dev);
		}
		else {
			usbi_dbg("device not found for session [%X]", session_id);
		}
	}
	usbi_mutex_static_unlock(&active_contexts_lock);
}

LRESULT CALLBACK message_callback_handle_device_change(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	DEV_BROADCAST_DEVICEINTERFACE_A *dev_bdi;
	HDEVINFO dev_info = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA dev_info_data = { 0 };
	char *device_id, *parent_device_id, *interface_path, *tmp;
	struct libusb_context *ctx;
	struct libusb_device *parent_dev;
	struct windows_device_priv *parent_priv;
	unsigned long session_id;
	int api, sub_api;
	unsigned int i, len;
#define MAX_ENUM_CLASS_LENGTH 4
	static const char *enum_classes[] = { "USB", "IUSB3", "NUSB3", "HID" };
	char enum_class[MAX_ENUM_CLASS_LENGTH + 1];
	bool connected, found = false;

	dev_bdi = (DEV_BROADCAST_DEVICEINTERFACE_A *)lParam;
	if (dev_bdi->dbcc_devicetype != DBT_DEVTYP_DEVICEINTERFACE) {
		return TRUE;
	}

	if ((wParam != DBT_DEVICEARRIVAL) && (wParam != DBT_DEVICEREMOVECOMPLETE)) {
		usbi_dbg("ignoring WM_DEVICECHANGE event %d", wParam);
		return TRUE;
	}

	device_id = parse_device_interface_path(dev_bdi->dbcc_name);
	if (device_id == NULL) {
		usbi_dbg("could not parse device interface path '%s'", dev_bdi->dbcc_name);
		return TRUE;
	}

	connected = (wParam == DBT_DEVICEARRIVAL);

	// This notification can be for a USB hub, USB device, or anything else
	// We can identify hubs and devices easily by the dbcc_classguid in the
	// DEV_BROADCAST_DEVICEINTERFACE structure. For these devices, the standard
	// enumeration process is used. Other device interfaces may or may not be
	// important. For example, we will want to process notifications for interfaces
	// of a composite USB device (e.g. installing WinUSB for that interface)
	if (guid_eq(&dev_bdi->dbcc_classguid, &GUID_DEVINTERFACE_USB_HUB) ||
		guid_eq(&dev_bdi->dbcc_classguid, &GUID_DEVINTERFACE_USB_DEVICE)) {
		usbi_dbg("PRO: %s (%s)", device_id, connected ? "CONNECTED" : "DISCONNECTED");

		if (connected) {
			windows_hotplug_enumerate(device_id, &dev_bdi->dbcc_classguid);
		}
		else {
			windows_hotplug_disconnect(device_id);
		}
	}
	else {
		// Before spending any time on this interface, check the enumerator class
		// to see if it is something we even care about. Notifications will be
		// received for every device interface in the system, so waste as little
		// time as possible
		tmp = strchr(device_id, '\\');
		if (tmp == NULL) {
			//usbi_dbg("'\' not found in device id '%s'", device_id);
			goto out;
		}

		len = tmp - device_id;
		if (len > MAX_ENUM_CLASS_LENGTH) {
			//usbi_dbg("ignoring notification for '%s'", device_id);
			goto out;
		}

		memcpy(enum_class, device_id, len);
		enum_class[len] = '\0';

		for (i = 0; i < ARRAYSIZE(enum_classes); i++) {
			if (strcmp(enum_class, enum_classes[i]) == 0) {
				found = true;
				break;
			}
		}
		if (!found) {
			//usbi_dbg("ignoring notification for '%s'", device_id);
			goto out;
		}

		usbi_dbg("IFC: %s %s (%s)", device_id, guid_to_string(&dev_bdi->dbcc_classguid),
			connected ? "CONNECTED" : "DISCONNECTED");

		if (!get_dev_info_data_by_device_id(NULL, &dev_info, &dev_info_data, device_id, connected)) {
			goto out;
		}

		parent_device_id = get_parent_device_id(dev_info_data.DevInst);
		if (parent_device_id == NULL) {
			usbi_dbg("could not get parent instance id for '%s'", device_id);
			goto out;
		}
		session_id = htab_hash(parent_device_id);
		free(parent_device_id);

		usbi_mutex_static_lock(&active_contexts_lock);
		list_for_each_entry(ctx, &active_contexts_list, list, struct libusb_context) {
			parent_dev = usbi_get_device_by_session_id(ctx, session_id);
			if (parent_dev != NULL) {
				// The parent device of this device interface is a USB device,
				// so let's add or remove the interface
				parent_priv = _device_priv(parent_dev);

				usbi_dbg("parent device '%s'", parent_priv->device_id);
				
				interface_path = sanitize_path(dev_bdi->dbcc_name);
				if (interface_path == NULL) {
					usbi_warn(ctx, "failed to sanitize interface path for '%s'", device_id);
					libusb_unref_device(parent_dev);
					continue;
				}

				if (connected) {
					if (parent_priv->apib->id == USB_API_COMPOSITE) {
						get_api_type(ctx, &dev_info, &dev_info_data, &api, &sub_api);
						if (api != USB_API_UNSUPPORTED) {
							if (set_composite_interface(parent_dev, interface_path, device_id,
								api, sub_api) == LIBUSB_SUCCESS) {
								interface_path = NULL;
							}
							else {
								usbi_warn(ctx, "failed to set composite interface for '%s'", device_id);
							}
						}
						else {
							usbi_dbg("unsupported API for interface '%s'", device_id);
						}
					}
					else if (parent_priv->apib->id == USB_API_HID) {
						if (set_hid_interface(parent_dev, interface_path) == LIBUSB_SUCCESS) {
							interface_path = NULL;
						}
						else {
							usbi_warn(ctx, "failed to set hid interface for '%s'", device_id);
						}
					}
				}
				else {
					if (parent_priv->apib->id == USB_API_COMPOSITE) {
						unset_composite_interface(parent_dev, device_id);
					}
					else if (parent_priv->apib->id == USB_API_HID) {
						unset_hid_interface(parent_dev, interface_path);
					}
				}

				safe_free(interface_path);
				libusb_unref_device(parent_dev);
			}
		}
		usbi_mutex_static_unlock(&active_contexts_lock);
	}

out:
	if (dev_info != INVALID_HANDLE_VALUE) {
		pSetupDiDestroyDeviceInfoList(dev_info);
	}
	free(device_id);
	return TRUE;
}

/*
 * Hotplug messaging callback
 */
// TODO: Windows limitations mean we cannot detect driverless devices with this
//		 => add another more generic callback for driverless, and check against
//			this one?
LRESULT CALLBACK messaging_callback(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	LRESULT ret = TRUE;

	switch (message) {
	case WM_DEVICECHANGE:
		ret = message_callback_handle_device_change(hWnd, message, wParam, lParam);
		break;
	default:
		ret = DefWindowProc(hWnd, message, wParam, lParam);
		break;
	}
	return ret;
}

unsigned __stdcall windows_hotplug_threaded(void *param)
{
	WNDCLASSEXA wc = { 0 };
	MSG msg;
	DEV_BROADCAST_DEVICEINTERFACE_A dev_bdi = { 0 };
	HDEVNOTIFY devnotify = NULL;
	BOOL msg_ret;
	int ret = 1;

#define LIBUSB_MSG_WINDOW_CLASS  "libusb_messaging_class"

	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = messaging_callback;
	wc.lpszClassName = LIBUSB_MSG_WINDOW_CLASS;

	if (!pRegisterClassExA(&wc)) {
		usbi_err(NULL, "can't register hotplug message window class %s", windows_error_str(0));
		goto err_exit;
	}

	// Note: Using HWND_MESSAGE removes broadcast events, like
	// the ones from driverless devices. However the broadcast
	// events you get on driverless provide no data whatsoever
	// about the device, the event (insertion or removal), or
	// even if the device is actually USB. Bummer!
	hHotplugMessage = CreateWindowExA(0, LIBUSB_MSG_WINDOW_CLASS, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
	if (hHotplugMessage == NULL) {
		usbi_err(NULL, "unable to create hotplug message window: %s", windows_error_str(0));
		goto err_exit;
	}

	dev_bdi.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE_A);
	dev_bdi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

	// Instead of registering specifically for the GUID_DEVINTERFACE_USB_DEVICE class
	// we will register to receive notifications for all interface classes. This is
	// the only way we will be able to tell if an interface to a composite or HID device
	// has changed, since these events won't trigger a notification message to the
	// USB device
	devnotify = pRegisterDeviceNotificationA(hHotplugMessage, &dev_bdi,
		DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
	if (devnotify == NULL) {
		usbi_err(NULL, "failed to register for device interface notification: %s", windows_error_str(0));
		goto err_exit;
	}

	usbi_dbg("hotplug thread waiting for messages");

	// Signal that the thread is ready
	hotplug_ready = TRUE;
	ReleaseSemaphore(hotplug_response, 1, NULL);

	// We need to handle the message pump
	while ((msg_ret = GetMessage(&msg, NULL, 0, 0)) != 0) {
		if (msg_ret == -1) {
			usbi_err(NULL, "GetMessage error: %s", windows_error_str(0));
		}
		else {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	ret = 0;
	goto cleanup;

err_exit:
	ReleaseSemaphore(hotplug_response, 1, NULL);
cleanup:
	if (devnotify != NULL) {
		pUnregisterDeviceNotification(devnotify);
	}
	if (hHotplugMessage != NULL) {
		DestroyWindow(hHotplugMessage);
		hHotplugMessage = NULL;
	}
	pUnregisterClassA(LIBUSB_MSG_WINDOW_CLASS, NULL);

	usbi_dbg("hotplug thread quitting");

	return ret;
}

/*
 * Scan and enumerate all USB hubs and devices
 *
 * Note: This function is only called when a context is created.
 */
void windows_scan_devices(struct libusb_context *ctx)
{
	HDEVINFO dev_info = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA dev_info_data = { 0 };
	unsigned int i;
	char *device_id = NULL;

	usbi_dbg("\n################## HUB pass ####################");
	for (i = 0;; i++) {
		safe_free(device_id);

		if (!get_dev_info_data_by_guid(ctx, &dev_info, &dev_info_data,
			&GUID_DEVINTERFACE_USB_HUB, i)) {
			break;
		}

		device_id = get_device_id(dev_info_data.DevInst);
		if (device_id == NULL) {
			continue;
		}

		windows_enumerate_device(ctx, device_id, &GUID_DEVINTERFACE_USB_HUB);
	}

	usbi_dbg("\n################## DEV pass ####################");
	for (i = 0;; i++) {
		safe_free(device_id);

		if (!get_dev_info_data_by_guid(ctx, &dev_info, &dev_info_data,
			&GUID_DEVINTERFACE_USB_DEVICE, i)) {
			break;
		}

		device_id = get_device_id(dev_info_data.DevInst);
		if (device_id == NULL) {
			continue;
		}

		windows_enumerate_device(ctx, device_id, &GUID_DEVINTERFACE_USB_DEVICE);
	}
}

/*
 * init: libusb backend init function
 *
 * This function enumerates all hubs and devices. Host Controller Drivers (HCD) are
 * saved as they encountered and are assigned a bus number based on the order in which
 * they are encountered.
 */
static int windows_init(struct libusb_context *ctx)
{
	int i, r = LIBUSB_ERROR_OTHER;
	HANDLE semaphore;
	char sem_name[11+1+8]; // strlen(libusb_init)+'\0'+(32-bit hex PID)

	sprintf(sem_name, "libusb_init%08X", (unsigned int)GetCurrentProcessId()&0xFFFFFFFF);
	semaphore = CreateSemaphoreA(NULL, 1, 1, sem_name);
	if (semaphore == NULL) {
		usbi_err(ctx, "could not create semaphore: %s", windows_error_str(0));
		return LIBUSB_ERROR_NO_MEM;
	}

	// A successful wait brings our semaphore count to 0 (unsignaled)
	// => any concurent wait stalls until the semaphore's release
	if (WaitForSingleObject(semaphore, INFINITE) != WAIT_OBJECT_0) {
		usbi_err(ctx, "failure to access semaphore: %s", windows_error_str(0));
		CloseHandle(semaphore);
		return LIBUSB_ERROR_NO_MEM;
	}

	// NB: concurrent usage supposes that init calls are equally balanced with
	// exit calls. If init is called more than exit, we will not exit properly
	if ( ++concurrent_usage == 0 ) {	// First init?
		get_windows_version();
		usbi_dbg(windows_version_str);
		if (windows_version == WINDOWS_UNSUPPORTED) {
			usbi_err(ctx, "This version of Windows is NOT supported");
			r = LIBUSB_ERROR_NOT_SUPPORTED;
			goto init_exit;
		}

		// We need a lock for proper auto-release
		if (usbi_mutex_init(&autoclaim_lock, NULL) != 0) {
			goto init_exit;
		}

		// We need a lock for host controller names
		if (usbi_mutex_init(&host_controller_lock, NULL) != 0) {
			goto init_exit;
		}

		// Load DLL imports
		if (init_dlls() != LIBUSB_SUCCESS) {
			usbi_err(ctx, "could not resolve DLL functions");
			return LIBUSB_ERROR_NOT_FOUND;
		}

		// Initialize the low level APIs (we don't care about errors at this stage)
		for (i=0; i<USB_API_MAX; i++) {
			usb_api_backend[i].init(SUB_API_NOTSET, ctx);
		}

		// Create a hash table to store session ids. Second parameter is
		// better if prime
		if( htab_create(ctx, HTAB_SIZE) == 0 ){
			goto init_exit;
		}

		// Initialize pollable file descriptors
		init_polling();

		// Because QueryPerformanceCounter might report different values when
		// running on different cores, we create a separate thread for the timer
		// calls, which we glue to the first core always to prevent timing discrepancies.
		r = LIBUSB_ERROR_NO_MEM;
		for (i = 0; i < 2; i++) {
			timer_request[i] = CreateEvent(NULL, TRUE, FALSE, NULL);
			if (timer_request[i] == NULL) {
				usbi_err(ctx, "could not create timer request event %d - aborting", i);
				goto init_exit;
			}
		}
		timer_response = CreateSemaphore(NULL, 0, MAX_TIMER_SEMAPHORES, NULL);
		if (timer_response == NULL) {
			usbi_err(ctx, "could not create timer response semaphore - aborting");
			goto init_exit;
		}
		timer_mutex = CreateMutex(NULL, FALSE, NULL);
		if (timer_mutex == NULL) {
			usbi_err(ctx, "could not create timer mutex - aborting");
			goto init_exit;
		}
		timer_thread = (HANDLE)_beginthreadex(NULL, 0, windows_clock_gettime_threaded, NULL, 0, NULL);
		if (timer_thread == NULL) {
			usbi_err(ctx, "Unable to create timer thread - aborting");
			goto init_exit;
		}
		SetThreadAffinityMask(timer_thread, 0);


		// Wait for timer thread to init before continuing.
		if (WaitForSingleObject(timer_response, INFINITE) != WAIT_OBJECT_0) {
			usbi_err(ctx, "Failed to wait for timer thread to become ready - aborting");
			goto init_exit;
		}

		hotplug_ready = FALSE;
		hotplug_response = CreateSemaphore(NULL, 0, 1, NULL);
		if (hotplug_response == NULL) {
			usbi_err(ctx, "could not create hotplug response semaphore - aborting");
			goto init_exit;
		}
		hotplug_thread = (HANDLE)_beginthreadex(NULL, 0, windows_hotplug_threaded, NULL, 0, NULL);
		if (hotplug_thread == NULL){
			usbi_err(ctx, "Unable to create hotplug thread - aborting");
			goto init_exit;
		}
		SetThreadAffinityMask(hotplug_thread, 0);

		// Wait for hotplug thread to init before continuing.
		if (WaitForSingleObject(hotplug_response, INFINITE) != WAIT_OBJECT_0) {
			usbi_err(ctx, "failed to wait for hotplug thread to become ready - aborting");
			goto init_exit;
		}
		if (hotplug_ready != TRUE) {
			usbi_err(ctx, "hotplug thread not ready - aborting");
			goto init_exit;
		}

	}

	// Initial enumeration
	windows_scan_devices(ctx);

	// At this stage, either we went through full init successfully, or didn't need to
	r = LIBUSB_SUCCESS;

init_exit: // Holds semaphore here.
	if (!concurrent_usage && r != LIBUSB_SUCCESS) { // First init failed?
		if (timer_thread) {
			SetEvent(timer_request[1]); // actually the signal to quit the thread.
			if (WAIT_OBJECT_0 != WaitForSingleObject(timer_thread, INFINITE)) {
				usbi_warn(ctx, "could not wait for timer thread to quit");
				TerminateThread(timer_thread, 1); // shouldn't happen, but we're destroying
												  // all objects it might have held anyway.
			}
			CloseHandle(timer_thread);
			timer_thread = NULL;
		}
		for (i = 0; i < 2; i++) {
			if (timer_request[i]) {
				CloseHandle(timer_request[i]);
				timer_request[i] = NULL;
			}
		}
		if (timer_response) {
			CloseHandle(timer_response);
			timer_response = NULL;
		}
		if (timer_mutex) {
			CloseHandle(timer_mutex);
			timer_mutex = NULL;
		}
		
		if (hotplug_thread) {
			PostMessage(hHotplugMessage, WM_QUIT, 0, 0);
			if (WAIT_OBJECT_0 != WaitForSingleObject(hotplug_thread, INFINITE)) {
				usbi_warn(ctx, "could not wait for hotplug thread to quit");
				TerminateThread(hotplug_thread, 1); // destroy it
			}
			CloseHandle(hotplug_thread);
			hotplug_thread = NULL;
		}
		if (hotplug_response) {
			CloseHandle(hotplug_response);
			hotplug_response = NULL;
		}
		htab_destroy();
		
		if (host_controller_lock) {
			usbi_mutex_destroy(&host_controller_lock);
		}
		if (autoclaim_lock) {
			usbi_mutex_destroy(&autoclaim_lock);
		}
	}

	if (r != LIBUSB_SUCCESS)
		--concurrent_usage; // Not expected to call libusb_exit if we failed.

	ReleaseSemaphore(semaphore, 1, NULL);	// increase count back to 1
	CloseHandle(semaphore);
	return r;
}

/*
 * HCD (root) hubs need to have their device descriptor manually populated
 *
 * Note that, like Microsoft does in the device manager, we populate the
 * Vendor and Device ID for HCD hubs with the ones from the PCI HCD device.
 */
static int force_hcd_device_descriptor(struct libusb_device *dev)
{
	struct libusb_context *ctx = DEVICE_CTX(dev);
	struct windows_device_priv *priv = _device_priv(dev);
	char *hcd_device_id;
	unsigned int index, vid, pid;
	int r = LIBUSB_ERROR_NO_DEVICE;

	hcd_device_id = get_parent_device_id(priv->devinst);
	if (hcd_device_id == NULL) {
		usbi_err(ctx, "could not retrieve host controller device id for root hub '%s'", priv->device_id);
		return LIBUSB_ERROR_NOT_FOUND;
	}

	// Windows does not exactly have bus numbers for host controllers like other platforms.
	// Traditionally the bus number has been derived from the host controller's position
	// in the dev_info set (from SetupAPI). However, changes to host controllers
	// (e.g. installing driver, enabling/disabling) can change the positions within the
	// dev_info set, which may introduce inconsistency across different contexts.
	// To ensure all contexts get the same bus numbers for the same host controllers, the
	// device instance ID of the host controller will be saved, and the bus number derived
	// from the order in which they are seen.
	usbi_mutex_lock(&host_controller_lock);
	for (index = 0; index < MAX_USB_HOST_CONTROLLERS; index++) {
		if (host_controller[index] == NULL) {
			host_controller[index] = _strdup(hcd_device_id);
			break;
		}
		else if (strcmp(host_controller[index], hcd_device_id) == 0) {
			break;
		}
	}
	usbi_mutex_unlock(&host_controller_lock);

	if (index == MAX_USB_HOST_CONTROLLERS) {
		usbi_err(ctx, "program assertion failed: too many host controllers");
		goto cleanup;
	}
	else if (host_controller[index] == NULL) {
		usbi_warn(ctx, "failed to assign bus number to host controller");
		goto cleanup;
	}

	dev->bus_number = (uint8_t)(index + 1);

	// Configure device descriptor
	dev->num_configurations = 1;
	priv->dev_descriptor.bLength = sizeof(USB_DEVICE_DESCRIPTOR);
	priv->dev_descriptor.bDescriptorType = USB_DEVICE_DESCRIPTOR_TYPE;
	priv->dev_descriptor.bNumConfigurations = 1;
	priv->dev_descriptor.bDeviceClass = LIBUSB_CLASS_HUB;
	priv->active_config = 1;

	if (sscanf(hcd_device_id, "PCI\\VEN_%04x&DEV_%04x%*s", &vid, &pid) == 2) {
		priv->dev_descriptor.idVendor = (uint16_t)vid;
		priv->dev_descriptor.idProduct = (uint16_t)pid;
	}
	else {
		usbi_warn(ctx, "could not infer VID/PID of HCD root hub from '%s'", hcd_device_id);
		priv->dev_descriptor.idVendor = 0x1d6b;		// Linux Foundation root hub
		priv->dev_descriptor.idProduct = 1;
	}

	r = LIBUSB_SUCCESS;

cleanup:
	free(hcd_device_id);

	return r;
}

/*
 * fetch and cache all the config descriptors through I/O
 */
static int cache_config_descriptors(struct libusb_device *dev, HANDLE hub_handle)
{
	DWORD size, ret_size;
	struct libusb_context *ctx = DEVICE_CTX(dev);
	struct windows_device_priv *priv = _device_priv(dev);
	int r;
	uint8_t i;

	USB_CONFIGURATION_DESCRIPTOR_SHORT cd_buf_short;    // dummy request
	PUSB_DESCRIPTOR_REQUEST cd_buf_actual = NULL;       // actual request
	PUSB_CONFIGURATION_DESCRIPTOR cd_data = NULL;

	if (dev->num_configurations == 0)
		return LIBUSB_ERROR_INVALID_PARAM;

	priv->config_descriptor = (unsigned char**) calloc(dev->num_configurations, sizeof(unsigned char*));
	if (priv->config_descriptor == NULL)
		return LIBUSB_ERROR_NO_MEM;
	for (i=0; i<dev->num_configurations; i++)
		priv->config_descriptor[i] = NULL;

	for (i=0, r=LIBUSB_SUCCESS; ; i++)
	{
		// safe loop: release all dynamic resources
		safe_free(cd_buf_actual);

		// safe loop: end of loop condition
		if ((i >= dev->num_configurations) || (r != LIBUSB_SUCCESS))
			break;

		size = sizeof(USB_CONFIGURATION_DESCRIPTOR_SHORT);
		memset(&cd_buf_short, 0, size);

		cd_buf_short.req.ConnectionIndex = (ULONG)priv->port;
		cd_buf_short.req.SetupPacket.bmRequest = LIBUSB_ENDPOINT_IN;
		cd_buf_short.req.SetupPacket.bRequest = USB_REQUEST_GET_DESCRIPTOR;
		cd_buf_short.req.SetupPacket.wValue = (USB_CONFIGURATION_DESCRIPTOR_TYPE << 8) | i;
		cd_buf_short.req.SetupPacket.wIndex = i;
		cd_buf_short.req.SetupPacket.wLength = (USHORT)(size - sizeof(USB_DESCRIPTOR_REQUEST));

		// Dummy call to get the required data size. Initial failures are reported as info rather
		// than error as they can occur for non-penalizing situations, such as with some hubs.
		// coverity[tainted_data_argument]
		if (!DeviceIoControl(hub_handle, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, &cd_buf_short, size,
			&cd_buf_short, size, &ret_size, NULL)) {
			usbi_info(ctx, "could not access configuration descriptor (dummy) for '%s': %s", priv->device_id, windows_error_str(0));
			LOOP_BREAK(LIBUSB_ERROR_IO);
		}

		if ((ret_size != size) || (cd_buf_short.data.wTotalLength < sizeof(USB_CONFIGURATION_DESCRIPTOR))) {
			usbi_info(ctx, "unexpected configuration descriptor size (dummy) for '%s'.", priv->device_id);
			LOOP_BREAK(LIBUSB_ERROR_IO);
		}

		size = sizeof(USB_DESCRIPTOR_REQUEST) + cd_buf_short.data.wTotalLength;
		if ((cd_buf_actual = (PUSB_DESCRIPTOR_REQUEST) calloc(1, size)) == NULL) {
			usbi_err(ctx, "could not allocate configuration descriptor buffer for '%s'.", priv->device_id);
			LOOP_BREAK(LIBUSB_ERROR_NO_MEM);
		}
		memset(cd_buf_actual, 0, size);

		// Actual call
		cd_buf_actual->ConnectionIndex = (ULONG)priv->port;
		cd_buf_actual->SetupPacket.bmRequest = LIBUSB_ENDPOINT_IN;
		cd_buf_actual->SetupPacket.bRequest = USB_REQUEST_GET_DESCRIPTOR;
		cd_buf_actual->SetupPacket.wValue = (USB_CONFIGURATION_DESCRIPTOR_TYPE << 8) | i;
		cd_buf_actual->SetupPacket.wIndex = i;
		cd_buf_actual->SetupPacket.wLength = (USHORT)(size - sizeof(USB_DESCRIPTOR_REQUEST));

		if (!DeviceIoControl(hub_handle, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, cd_buf_actual, size,
			cd_buf_actual, size, &ret_size, NULL)) {
			usbi_err(ctx, "could not access configuration descriptor (actual) for '%s': %s", priv->device_id, windows_error_str(0));
			LOOP_BREAK(LIBUSB_ERROR_IO);
		}

		cd_data = (PUSB_CONFIGURATION_DESCRIPTOR)((UCHAR*)cd_buf_actual+sizeof(USB_DESCRIPTOR_REQUEST));

		if ((size != ret_size) || (cd_data->wTotalLength != cd_buf_short.data.wTotalLength)) {
			usbi_err(ctx, "unexpected configuration descriptor size (actual) for '%s'.", priv->device_id);
			LOOP_BREAK(LIBUSB_ERROR_IO);
		}

		if (cd_data->bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE) {
			usbi_err(ctx, "not a configuration descriptor for '%s'", priv->device_id);
			LOOP_BREAK(LIBUSB_ERROR_IO);
		}

		usbi_dbg("cached config descriptor %d (bConfigurationValue=%d, %d bytes)",
			i, cd_data->bConfigurationValue, cd_data->wTotalLength);

		// Cache the descriptor
		priv->config_descriptor[i] = (unsigned char*) malloc(cd_data->wTotalLength);
		if (priv->config_descriptor[i] == NULL)
			LOOP_BREAK(LIBUSB_ERROR_NO_MEM);
		memcpy(priv->config_descriptor[i], cd_data, cd_data->wTotalLength);
	}
	return LIBUSB_SUCCESS;
}

/*
 * Populate a libusb device structure
 */
static int init_device(struct libusb_device *dev, struct libusb_device *parent_dev, uint8_t port_number)
{
	HANDLE handle;
	DWORD size;
	USB_NODE_CONNECTION_INFORMATION_EX conn_info;
	USB_NODE_CONNECTION_INFORMATION_EX_V2 conn_info_v2;
	struct windows_device_priv *priv, *parent_priv;
	struct libusb_context *ctx = DEVICE_CTX(dev);
	int res;

	usbi_dbg("");

	priv = _device_priv(dev);

	// Port number of zero means this is a root hub
	if (port_number == 0) {
		if (parent_dev != NULL) {
			usbi_err(ctx, "program assertion failed: device has port 0 and a parent");
			return LIBUSB_ERROR_OTHER;
		}
		dev->device_address = 1; // root hubs get this address
		dev->port_number = 0;
		priv->port = 0;
		priv->depth = 0;
		force_hcd_device_descriptor(dev);
		if (dev->bus_number == 0) {
			usbi_err(ctx, "program assertion failed: unable to determine bus number of root hub '%s'", priv->device_id);
			return LIBUSB_ERROR_NOT_FOUND;
		}
	}
	else {
		if (parent_dev == NULL) {
			return LIBUSB_ERROR_NOT_FOUND;
		}

		parent_priv = _device_priv(parent_dev);
		if (parent_priv->apib->id != USB_API_HUB) {
			usbi_warn(ctx, "parent for device '%s' is not a hub", priv->device_id);
			return LIBUSB_ERROR_NOT_FOUND;
		}

		if (parent_dev->bus_number == 0) {
			usbi_err(ctx, "program assertion failed: parent device bus number not set for '%s'", priv->device_id);
			return LIBUSB_ERROR_NOT_FOUND;
		}

		dev->bus_number = parent_dev->bus_number;
		priv->port = port_number;
		dev->port_number = port_number;
		priv->depth = parent_priv->depth + 1;

		// If the device address is already set, we can stop here
		if (dev->device_address != 0) {
			dev->parent_dev = libusb_ref_device(parent_dev);
			return LIBUSB_SUCCESS;
		}

		memset(&conn_info, 0, sizeof(conn_info));
		handle = CreateFileA(parent_priv->path, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED, NULL);
		if (handle == INVALID_HANDLE_VALUE) {
			usbi_warn(ctx, "could not open hub '%s': %s", parent_priv->path, windows_error_str(0));
			return LIBUSB_ERROR_ACCESS;
		}
		size = sizeof(conn_info);
		conn_info.ConnectionIndex = (ULONG)port_number;
		// coverity[tainted_data_argument]
		if (!DeviceIoControl(handle, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, &conn_info, size,
			&conn_info, size, &size, NULL)) {
			usbi_warn(ctx, "could not get node connection information for device '%s': %s",
				priv->device_id, windows_error_str(0));
			safe_closehandle(handle);
			return LIBUSB_ERROR_NO_DEVICE;
		}
		if (conn_info.ConnectionStatus == NoDeviceConnected) {
			usbi_err(ctx, "device '%s' is no longer connected!", priv->device_id);
			safe_closehandle(handle);
			return LIBUSB_ERROR_NO_DEVICE;
		}
		memcpy(&priv->dev_descriptor, &(conn_info.DeviceDescriptor), sizeof(USB_DEVICE_DESCRIPTOR));
		dev->num_configurations = priv->dev_descriptor.bNumConfigurations;
		priv->active_config = conn_info.CurrentConfigurationValue;
		usbi_dbg("found %d configuration(s) (active conf: %d)", dev->num_configurations, priv->active_config);
		// If we can't read the config descriptors, just set the number of confs to zero
		res = cache_config_descriptors(dev, handle);
		if (res != LIBUSB_SUCCESS) {
			dev->num_configurations = 0;
			priv->dev_descriptor.bNumConfigurations = 0;
		}

		// In their great wisdom, Microsoft decided to BREAK the USB speed report between Windows 7 and Windows 8
		if (windows_version >= WINDOWS_8) {
			memset(&conn_info_v2, 0, sizeof(conn_info_v2));
			size = sizeof(conn_info_v2);
			conn_info_v2.ConnectionIndex = (ULONG)port_number;
			conn_info_v2.Length = size;
			conn_info_v2.SupportedUsbProtocols.Usb300 = 1;
			if (!DeviceIoControl(handle, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2,
				&conn_info_v2, size, &conn_info_v2, size, &size, NULL)) {
				usbi_warn(ctx, "could not get node connection information (V2) for device '%s': %s",
					priv->device_id,  windows_error_str(0));
			} else if (conn_info_v2.Flags.DeviceIsOperatingAtSuperSpeedOrHigher) {
				conn_info.Speed = 3;
			}
		}

		safe_closehandle(handle);

		if (conn_info.DeviceAddress > UINT8_MAX) {
			usbi_err(ctx, "program assertion failed: device address overflow");
		}
		dev->device_address = (uint8_t)conn_info.DeviceAddress + 1;
		if (dev->device_address == 1) {
			usbi_err(ctx, "program assertion failed: device address collision with root hub");
		}
		switch (conn_info.Speed) {
		case 0: dev->speed = LIBUSB_SPEED_LOW; break;
		case 1: dev->speed = LIBUSB_SPEED_FULL; break;
		case 2: dev->speed = LIBUSB_SPEED_HIGH; break;
		case 3: dev->speed = LIBUSB_SPEED_SUPER; break;
		default:
			usbi_warn(ctx, "Got unknown device speed %d", conn_info.Speed);
			break;
		}
		dev->parent_dev = libusb_ref_device(parent_dev);
	}

	usbi_sanitize_device(dev);

	usbi_dbg("(bus: %d, addr: %d, depth: %d, port: %d): '%s'",
			 dev->bus_number, dev->device_address,
			 priv->depth, priv->port, priv->device_id);

	return LIBUSB_SUCCESS;
}

// Returns the api type, or 0 if not found/unsupported
static void get_api_type(struct libusb_context *ctx,
	HDEVINFO *dev_info, SP_DEVINFO_DATA *dev_info_data, int *api, int *sub_api)
{
	// Precedence for filter drivers vs driver is in the order of this array
	struct driver_lookup lookup[3] = {
		{"\0\0", SPDRP_SERVICE, "driver"},
		{"\0\0", SPDRP_UPPERFILTERS, "upper filter driver"},
		{"\0\0", SPDRP_LOWERFILTERS, "lower filter driver"}
	};
	DWORD size;
	unsigned k, l;
	int i, j;

	*api = USB_API_UNSUPPORTED;
	*sub_api = SUB_API_NOTSET;
	// Check the service & filter names to know the API we should use
	for (k=0; k<3; k++) {
		if (pSetupDiGetDeviceRegistryPropertyA(*dev_info, dev_info_data, lookup[k].reg_prop,
			NULL, (BYTE*)lookup[k].list, MAX_KEY_LENGTH, &size)) {
			// Turn the REG_SZ SPDRP_SERVICE into REG_MULTI_SZ
			if (lookup[k].reg_prop == SPDRP_SERVICE) {
				// our buffers are MAX_KEY_LENGTH+1 so we can overflow if needed
				lookup[k].list[safe_strlen(lookup[k].list)+1] = 0;
			}
			// MULTI_SZ is a pain to work with. Turn it into something much more manageable
			// NB: none of the driver names we check against contain LIST_SEPARATOR,
			// (currently ';'), so even if an unsuported one does, it's not an issue
			for (l=0; (lookup[k].list[l] != 0) || (lookup[k].list[l+1] != 0); l++) {
				if (lookup[k].list[l] == 0) {
					lookup[k].list[l] = LIST_SEPARATOR;
				}
			}
			usbi_dbg("%s(s): %s", lookup[k].designation, lookup[k].list);
		} else {
			if (GetLastError() != ERROR_INVALID_DATA) {
				usbi_dbg("could not access %s: %s", lookup[k].designation, windows_error_str(0));
			}
			lookup[k].list[0] = 0;
		}
	}

	for (i=1; i<USB_API_MAX; i++) {
		for (k=0; k<3; k++) {
			j = get_sub_api(lookup[k].list, i);
			if (j >= 0) {
				usbi_dbg("matched %s name against %s",
					lookup[k].designation, (i!=USB_API_WINUSBX)?usb_api_backend[i].designation:sub_api_name[j]);
				*api = i;
				*sub_api = j;
				return;
			}
		}
	}
}

static int enumerate_device_interfaces(struct libusb_device *dev) {
	DWORD child_devinst, size;
	HDEVINFO dev_info = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA dev_info_data = { 0 };
	SP_DEVICE_INTERFACE_DETAIL_DATA_A *dev_interface_detail = NULL;
	HKEY key;
	WCHAR interface_guid_string[MAX_GUID_STRING_LENGTH];
	GUID *interface_guid[USB_MAXINTERFACES];
	GUID *tmp;
	LONG res;
	struct libusb_context *ctx = DEVICE_CTX(dev);
	struct windows_device_priv *priv = _device_priv(dev);
	char *device_id = NULL, *dev_interface_path = NULL;
	int api, sub_api;
	unsigned int index, guid_index, nb_guids = 1;

	if ((priv->apib->id != USB_API_COMPOSITE) && (priv->apib->id != USB_API_HID)) {
		usbi_err(ctx, "program assertion failed: '%s' is not composite/hid", priv->device_id);
		return LIBUSB_ERROR_NOT_FOUND;
	}

	if ((priv->apib->id == USB_API_HID) && (priv->hid == NULL)) {
		usbi_err(ctx, "program assertion failed: '%s' is not hid", priv->device_id);
		return LIBUSB_ERROR_NOT_FOUND;
	}

	interface_guid[0] = &HID_guid;

	// For composite devices, search for device interface GUIDs on all interfaces
	if (priv->apib->id == USB_API_COMPOSITE) {
		if (CM_Get_Child(&child_devinst, priv->devinst, 0) != CR_SUCCESS) {
			usbi_warn(ctx, "could not find child for composite/hid device '%s'", priv->device_id);
			return LIBUSB_ERROR_NOT_FOUND;
		}

		// Loop over all child nodes of this device by searching through siblings
		// The loop ends when no more siblings are encountered
		for (;;) {
			device_id = get_device_id(child_devinst);
			if (device_id == NULL) {
				usbi_warn(ctx, "failed to get device instance id for instance %d", child_devinst);
				goto next_child;
			}

			if (!get_dev_info_data_by_device_id(ctx, &dev_info, &dev_info_data, device_id, TRUE)) {
				goto next_child;
			}

			// Check to see if this interface has a device interface GUID
			key = pSetupDiOpenDevRegKey(dev_info, &dev_info_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
			if (key != INVALID_HANDLE_VALUE) {
				size = sizeof(interface_guid_string);
				res = pRegQueryValueExW(key, L"DeviceInterfaceGUIDs", NULL, NULL, (BYTE *)interface_guid_string, &size);
				pRegCloseKey(key);
				if (res == ERROR_SUCCESS) {
					tmp = (GUID *)calloc(1, sizeof(GUID));
					if (tmp == NULL) {
						usbi_warn(ctx, "failed to allocate device interface GUID");
						continue;
					}
					pCLSIDFromString(interface_guid_string, tmp);
					interface_guid[nb_guids++] = tmp;
					usbi_dbg("'%s' has interface GUID %s", device_id, guid_to_string(tmp));
				}
			}

		next_child:
			if (dev_info != INVALID_HANDLE_VALUE) {
				pSetupDiDestroyDeviceInfoList(dev_info);
				dev_info = INVALID_HANDLE_VALUE;
			}
			safe_free(device_id);

			if (CM_Get_Sibling(&child_devinst, child_devinst, 0) != CR_SUCCESS) {
				break;
			}
		}
	}

	// Now loop over all device interface GUIDs, starting with HID
	// Note: Only composite devices can possibly have more than one
	// device interface GUID to search
	for (guid_index = 0; guid_index < nb_guids; guid_index++) {
		for (index = 0;; index++) {
			safe_free(device_id);
			safe_free(dev_interface_detail);
			safe_free(dev_interface_path);

			dev_interface_detail = get_interface_detail(ctx, &dev_info, &dev_info_data,
				NULL, interface_guid[guid_index], index);

			if (dev_interface_detail == NULL) {
				break;
			}

			device_id = get_device_id(dev_info_data.DevInst);
			if (device_id == NULL) {
				usbi_warn(ctx, "failed to get device instance id for instance %d", dev_info_data.DevInst);
				continue;
			}

			// Device interfaces can belong to any device, so make sure this interface is a child node
			// of the device are enumerating
			if (get_parent_device_instance(dev_info_data.DevInst) == priv->devinst) {
				dev_interface_path = sanitize_path(dev_interface_detail->DevicePath);
				if (dev_interface_path == NULL) {
					usbi_warn(ctx, "failed to sanitize interface path for '%s'", device_id);
					continue;
				}

				if (priv->apib->id == USB_API_COMPOSITE) {
					get_api_type(ctx, &dev_info, &dev_info_data, &api, &sub_api);
					if (api == USB_API_UNSUPPORTED) {
						continue;
					}

					if (set_composite_interface(dev, dev_interface_path, device_id, api, sub_api) != LIBUSB_SUCCESS) {
						usbi_warn(ctx, "failed to set composite interface for '%s'", device_id);
						continue;
					}
					dev_interface_path = NULL;
				}
				else if (priv->apib->id == USB_API_HID) {
					if (set_hid_interface(dev, dev_interface_path) != LIBUSB_SUCCESS) {
						usbi_warn(ctx, "failed to set composite interface for '%s'", device_id);
						continue;
					}
					dev_interface_path = NULL;
				}
			}
		}
	}

	// Free any GUIDs that were allocated
	for (guid_index = 1; guid_index < nb_guids; guid_index++) {
		safe_free(interface_guid[guid_index]);
	}

	return LIBUSB_SUCCESS;
}

static int set_composite_interface(struct libusb_device *dev, char *dev_interface_path, char *device_id,
	int api, int sub_api)
{
	unsigned i;
	struct libusb_context *ctx = DEVICE_CTX(dev);
	struct windows_device_priv *priv = _device_priv(dev);
	int interface_number;

	if (priv->apib->id != USB_API_COMPOSITE) {
		usbi_err(ctx, "program assertion failed: '%s' is not composite", priv->device_id);
		return LIBUSB_ERROR_NO_DEVICE;
	}

	// Because MI_## are not necessarily in sequential order (some composite
	// devices will have only MI_00 & MI_03 for instance), we retrieve the actual
	// interface number from the path's MI value
	interface_number = 0;
	for (i=0; device_id[i] != 0; ) {
		if ( (device_id[i++] == 'M') && (device_id[i++] == 'I')
		  && (device_id[i++] == '_') ) {
			interface_number = (device_id[i++] - '0')*10;
			interface_number += device_id[i] - '0';
			break;
		}
	}

	if (device_id[i] == 0) {
		usbi_warn(ctx, "failure to read interface number for %s. Using default value %d",
			device_id, interface_number);
	}

	if (interface_number >= USB_MAXINTERFACES) {
		usbi_err(ctx, "program assertion failed: max USB interface number exceeded");
		return LIBUSB_ERROR_OTHER;
	}

	if (priv->usb_interface[interface_number].path != NULL) {
		if (api == USB_API_HID) {
			// HID devices can have multiple collections (COL##) for each MI_## interface
			usbi_dbg("interface[%d] already set - ignoring HID collection: %s",
				interface_number, device_id);
			return LIBUSB_ERROR_ACCESS;
		}
		// In other cases, just use the latest data
		safe_free(priv->usb_interface[interface_number].path);
	}

	usbi_dbg("interface[%d] = %s", interface_number, dev_interface_path);
	priv->usb_interface[interface_number].path = dev_interface_path;
	priv->usb_interface[interface_number].apib = &usb_api_backend[api];
	priv->usb_interface[interface_number].sub_api = sub_api;
	if ((api == USB_API_HID) && (priv->hid == NULL)) {
		priv->hid = (struct hid_device_priv*) calloc(1, sizeof(struct hid_device_priv));
		if (priv->hid == NULL)
			return LIBUSB_ERROR_NO_MEM;
	}

	return LIBUSB_SUCCESS;
}

static int set_hid_interface(struct libusb_device *dev, char *dev_interface_path)
{
	int i;
	struct libusb_context *ctx = DEVICE_CTX(dev);
	struct windows_device_priv *priv = _device_priv(dev);

	if (priv->hid == NULL) {
		usbi_err(ctx, "program assertion failed: parent is not HID");
		return LIBUSB_ERROR_NO_DEVICE;
	}
	if (priv->hid->nb_interfaces == USB_MAXINTERFACES) {
		usbi_err(ctx, "program assertion failed: max USB interfaces reached for HID device");
		return LIBUSB_ERROR_NO_DEVICE;
	}
	for (i=0; i<priv->hid->nb_interfaces; i++) {
		if (safe_strcmp(priv->usb_interface[i].path, dev_interface_path) == 0) {
			usbi_dbg("interface[%d] already set to %s", i, dev_interface_path);
			return LIBUSB_SUCCESS;
		}
	}

	priv->usb_interface[priv->hid->nb_interfaces].path = dev_interface_path;
	priv->usb_interface[priv->hid->nb_interfaces].apib = &usb_api_backend[USB_API_HID];
	usbi_dbg("interface[%d] = %s", priv->hid->nb_interfaces, dev_interface_path);
	priv->hid->nb_interfaces++;
	return LIBUSB_SUCCESS;
}

static void unset_composite_interface(struct libusb_device *dev, char *device_id)
{
	unsigned i;
	struct libusb_context *ctx = DEVICE_CTX(dev);
	struct windows_device_priv *priv = _device_priv(dev);
	int interface_number;
	bool hid_present = false;

	if (priv->apib->id != USB_API_COMPOSITE) {
		usbi_err(ctx, "program assertion failed: '%s' is not composite", priv->device_id);
		return;
	}

	// Because MI_## are not necessarily in sequential order (some composite
	// devices will have only MI_00 & MI_03 for instance), we retrieve the actual
	// interface number from the path's MI value
	interface_number = 0;
	for (i = 0; device_id[i] != 0;) {
		if ((device_id[i++] == 'M') && (device_id[i++] == 'I')
			&& (device_id[i++] == '_')) {
			interface_number = (device_id[i++] - '0') * 10;
			interface_number += device_id[i] - '0';
			break;
		}
	}

	if (device_id[i] == 0) {
		usbi_warn(ctx, "failure to read interface number for %s. Using default value %d",
			device_id, interface_number);
	}

	if (interface_number >= USB_MAXINTERFACES) {
		usbi_err(ctx, "program assertion failed: max USB interface number exceeded");
		return;
	}

	// Interface is not necessarily set if it doesn't have a supported driver
	if (priv->usb_interface[interface_number].path == NULL) {
		return;
	}

	usbi_dbg("removing interface[%d] = %s", interface_number, priv->usb_interface[interface_number].path);
	safe_free(priv->usb_interface[interface_number].path);
	priv->usb_interface[interface_number].apib = &usb_api_backend[USB_API_UNSUPPORTED];
	priv->usb_interface[interface_number].sub_api = SUB_API_NOTSET;

	for (i = 0; i < USB_MAXINTERFACES; i++) {
		if (priv->usb_interface[i].apib->id == USB_API_HID) {
			hid_present = true;
		}
	}

	if (!hid_present) {
		safe_free(priv->hid);
	}
}

static void unset_hid_interface(struct libusb_device *dev, char *dev_interface_path)
{
	unsigned interface_number;
	struct libusb_context *ctx = DEVICE_CTX(dev);
	struct windows_device_priv *priv = _device_priv(dev);

	if (priv->hid == NULL) {
		usbi_err(ctx, "program assertion failed: parent is not HID");
		return;
	}

	for (interface_number = 0; interface_number < priv->hid->nb_interfaces; interface_number++) {
		if (priv->usb_interface[interface_number].path == NULL) {
			usbi_err(ctx, "program assertion failed: hid interface path not set");
			continue;
		}
		if (strcmp(priv->usb_interface[interface_number].path, dev_interface_path) == 0) {
			break;
		}
	}

	if (interface_number == priv->hid->nb_interfaces) {
		// HID interface not found
		return;
	}

	usbi_dbg("removing interface[%d] = %s", interface_number, dev_interface_path);
	safe_free(priv->usb_interface[interface_number].path);
	
	// Shift all remaining interfaces up in the list
	for (; interface_number < (priv->hid->nb_interfaces - 1); interface_number++) {
		memcpy(&priv->usb_interface[interface_number], &priv->usb_interface[interface_number + 1], sizeof(priv->usb_interface[interface_number]));
	}

	memset(&priv->usb_interface[interface_number], 0, sizeof(priv->usb_interface[interface_number]));
	priv->usb_interface[interface_number].apib = &usb_api_backend[USB_API_UNSUPPORTED];
	priv->usb_interface[interface_number].sub_api = SUB_API_NOTSET;
	priv->hid->nb_interfaces--;
}

/*
 * exit: libusb backend deinitialization function
 */
static void windows_exit(void)
{
	int i;
	HANDLE semaphore;
	DWORD ret;
	char sem_name[11+1+8]; // strlen(libusb_init)+'\0'+(32-bit hex PID)

	sprintf(sem_name, "libusb_init%08X", (unsigned int)GetCurrentProcessId()&0xFFFFFFFF);
	semaphore = CreateSemaphoreA(NULL, 1, 1, sem_name);
	if (semaphore == NULL) {
		return;
	}

	// A successful wait brings our semaphore count to 0 (unsignaled)
	// => any concurent wait stalls until the semaphore release
	ret = WaitForSingleObject(semaphore, INFINITE);
	if ( ret != WAIT_OBJECT_0) {
		CloseHandle(semaphore);
		return;
	}

	// Only works if exits and inits are balanced exactly
	if (--concurrent_usage < 0) {	// Last exit
		for (i=0; i<USB_API_MAX; i++) {
			usb_api_backend[i].exit(SUB_API_NOTSET);
		}
		exit_polling();

		if (timer_thread) {
			// actually the signal to quit the thread.
			SetEvent(timer_request[1]);
			ret = WaitForSingleObject(timer_thread, INFINITE);
			if (WAIT_OBJECT_0 != ret) {
				usbi_dbg("could not wait for timer thread to quit");
				TerminateThread(timer_thread, 1);
			}
			CloseHandle(timer_thread);
			timer_thread = NULL;
		}
		for (i = 0; i < 2; i++) {
			if (timer_request[i]) {
				CloseHandle(timer_request[i]);
				timer_request[i] = NULL;
			}
		}
		if (timer_response) {
			CloseHandle(timer_response);
			timer_response = NULL;
		}
		if (timer_mutex) {
			CloseHandle(timer_mutex);
			timer_mutex = NULL;
		}

		if (hHotplugMessage) {
			// signal thread to exit
			PostMessage(hHotplugMessage, WM_QUIT, 0, 0);
			if (WAIT_OBJECT_0 != WaitForSingleObject(hotplug_thread, INFINITE)) {
				usbi_dbg("could not wait for hotplug thread to quit");
				TerminateThread(hotplug_thread, 1);
			}
			CloseHandle(hotplug_thread);
			hotplug_thread = NULL;
			hHotplugMessage = NULL;
		}
		if (hotplug_response) {
			CloseHandle(hotplug_response);
			hotplug_response = NULL;
		}
		htab_destroy();

		for (i = 0; i < MAX_USB_HOST_CONTROLLERS; i++) {
			safe_free(host_controller[i]);
		}

		if (host_controller_lock) {
			usbi_mutex_destroy(&host_controller_lock);
		}
		if (autoclaim_lock) {
			usbi_mutex_destroy(&autoclaim_lock);
		}
	}

	ReleaseSemaphore(semaphore, 1, NULL);	// increase count back to 1
	CloseHandle(semaphore);
}

static int windows_get_device_descriptor(struct libusb_device *dev, unsigned char *buffer, int *host_endian)
{
	struct windows_device_priv *priv = _device_priv(dev);

	memcpy(buffer, &(priv->dev_descriptor), DEVICE_DESC_LENGTH);
	*host_endian = 0;

	return LIBUSB_SUCCESS;
}

static int windows_get_config_descriptor(struct libusb_device *dev, uint8_t config_index, unsigned char *buffer, size_t len, int *host_endian)
{
	struct windows_device_priv *priv = _device_priv(dev);
	PUSB_CONFIGURATION_DESCRIPTOR config_header;
	size_t size;

	// config index is zero based
	if (config_index >= dev->num_configurations)
		return LIBUSB_ERROR_INVALID_PARAM;

	if ((priv->config_descriptor == NULL) || (priv->config_descriptor[config_index] == NULL))
		return LIBUSB_ERROR_NOT_FOUND;

	config_header = (PUSB_CONFIGURATION_DESCRIPTOR)priv->config_descriptor[config_index];

	size = min(config_header->wTotalLength, len);
	memcpy(buffer, priv->config_descriptor[config_index], size);
	*host_endian = 0;

	return (int)size;
}

/*
 * return the cached copy of the active config descriptor
 */
static int windows_get_active_config_descriptor(struct libusb_device *dev, unsigned char *buffer, size_t len, int *host_endian)
{
	struct windows_device_priv *priv = _device_priv(dev);

	if (priv->active_config == 0)
		return LIBUSB_ERROR_NOT_FOUND;

	// config index is zero based
	return windows_get_config_descriptor(dev, (uint8_t)(priv->active_config-1), buffer, len, host_endian);
}

static int windows_open(struct libusb_device_handle *dev_handle)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);

	if (priv->apib == NULL) {
		usbi_err(ctx, "program assertion failed - device is not initialized");
		return LIBUSB_ERROR_NO_DEVICE;
	}

	return priv->apib->open(SUB_API_NOTSET, dev_handle);
}

static void windows_close(struct libusb_device_handle *dev_handle)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);

	priv->apib->close(SUB_API_NOTSET, dev_handle);
}

static int windows_get_configuration(struct libusb_device_handle *dev_handle, int *config)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);

	if (priv->active_config == 0) {
		*config = 0;
		return LIBUSB_ERROR_NOT_FOUND;
	}

	*config = priv->active_config;
	return LIBUSB_SUCCESS;
}

/*
 * from http://msdn.microsoft.com/en-us/library/ms793522.aspx: "The port driver
 * does not currently expose a service that allows higher-level drivers to set
 * the configuration."
 */
static int windows_set_configuration(struct libusb_device_handle *dev_handle, int config)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	int r = LIBUSB_SUCCESS;

	if (config >= USB_MAXCONFIG)
		return LIBUSB_ERROR_INVALID_PARAM;

	r = libusb_control_transfer(dev_handle, LIBUSB_ENDPOINT_OUT |
		LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
		LIBUSB_REQUEST_SET_CONFIGURATION, (uint16_t)config,
		0, NULL, 0, 1000);

	if (r == LIBUSB_SUCCESS) {
		priv->active_config = (uint8_t)config;
	}
	return r;
}

static int windows_claim_interface(struct libusb_device_handle *dev_handle, int iface)
{
	int r = LIBUSB_SUCCESS;
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);

	if (iface >= USB_MAXINTERFACES)
		return LIBUSB_ERROR_INVALID_PARAM;

	safe_free(priv->usb_interface[iface].endpoint);
	priv->usb_interface[iface].nb_endpoints= 0;

	r = priv->apib->claim_interface(SUB_API_NOTSET, dev_handle, iface);

	if (r == LIBUSB_SUCCESS) {
		r = windows_assign_endpoints(dev_handle, iface, 0);
	}

	return r;
}

static int windows_set_interface_altsetting(struct libusb_device_handle *dev_handle, int iface, int altsetting)
{
	int r = LIBUSB_SUCCESS;
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);

	safe_free(priv->usb_interface[iface].endpoint);
	priv->usb_interface[iface].nb_endpoints= 0;

	r = priv->apib->set_interface_altsetting(SUB_API_NOTSET, dev_handle, iface, altsetting);

	if (r == LIBUSB_SUCCESS) {
		r = windows_assign_endpoints(dev_handle, iface, altsetting);
	}

	return r;
}

static int windows_release_interface(struct libusb_device_handle *dev_handle, int iface)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);

	return priv->apib->release_interface(SUB_API_NOTSET, dev_handle, iface);
}

static int windows_clear_halt(struct libusb_device_handle *dev_handle, unsigned char endpoint)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	return priv->apib->clear_halt(SUB_API_NOTSET, dev_handle, endpoint);
}

static int windows_reset_device(struct libusb_device_handle *dev_handle)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	return priv->apib->reset_device(SUB_API_NOTSET, dev_handle);
}

// The 3 functions below are unlikely to ever get supported on Windows
static int windows_kernel_driver_active(struct libusb_device_handle *dev_handle, int iface)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

static int windows_attach_kernel_driver(struct libusb_device_handle *dev_handle, int iface)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

static int windows_detach_kernel_driver(struct libusb_device_handle *dev_handle, int iface)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

static void windows_destroy_device(struct libusb_device *dev)
{
	struct windows_device_priv *priv;

	if (dev == NULL) {
		return;
	}

	priv = _device_priv(dev);
	if (priv == NULL){
		return;
	}

	windows_device_priv_release(dev);
}

static void windows_clear_transfer_priv(struct usbi_transfer *itransfer)
{
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);

	usbi_free_fd(&transfer_priv->pollable_fd);
	safe_free(transfer_priv->hid_buffer);
	// When auto claim is in use, attempt to release the auto-claimed interface
	auto_release(itransfer);
}

static int submit_bulk_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	int r;

	r = priv->apib->submit_bulk_transfer(SUB_API_NOTSET, itransfer);
	if (r != LIBUSB_SUCCESS) {
		return r;
	}

	usbi_add_pollfd(ctx, transfer_priv->pollable_fd.fd,
		(short)(IS_XFERIN(transfer) ? POLLIN : POLLOUT));

	itransfer->flags |= USBI_TRANSFER_UPDATED_FDS;
	return LIBUSB_SUCCESS;
}

static int submit_iso_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	int r;

	r = priv->apib->submit_iso_transfer(SUB_API_NOTSET, itransfer);
	if (r != LIBUSB_SUCCESS) {
		return r;
	}

	usbi_add_pollfd(ctx, transfer_priv->pollable_fd.fd,
		(short)(IS_XFERIN(transfer) ? POLLIN : POLLOUT));

	itransfer->flags |= USBI_TRANSFER_UPDATED_FDS;
	return LIBUSB_SUCCESS;
}

static int submit_control_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	int r;

	r = priv->apib->submit_control_transfer(SUB_API_NOTSET, itransfer);
	if (r != LIBUSB_SUCCESS) {
		return r;
	}

	usbi_add_pollfd(ctx, transfer_priv->pollable_fd.fd, POLLIN);

	itransfer->flags |= USBI_TRANSFER_UPDATED_FDS;
	return LIBUSB_SUCCESS;

}

static int windows_submit_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);

	switch (transfer->type) {
	case LIBUSB_TRANSFER_TYPE_CONTROL:
		return submit_control_transfer(itransfer);
	case LIBUSB_TRANSFER_TYPE_BULK:
	case LIBUSB_TRANSFER_TYPE_INTERRUPT:
		if (IS_XFEROUT(transfer) &&
		    transfer->flags & LIBUSB_TRANSFER_ADD_ZERO_PACKET)
			return LIBUSB_ERROR_NOT_SUPPORTED;
		return submit_bulk_transfer(itransfer);
	case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
		return submit_iso_transfer(itransfer);
	case LIBUSB_TRANSFER_TYPE_BULK_STREAM:
		return LIBUSB_ERROR_NOT_SUPPORTED;
	default:
		usbi_err(TRANSFER_CTX(transfer), "unknown endpoint type %d", transfer->type);
		return LIBUSB_ERROR_INVALID_PARAM;
	}
}

static int windows_abort_control(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);

	return priv->apib->abort_control(SUB_API_NOTSET, itransfer);
}

static int windows_abort_transfers(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);

	return priv->apib->abort_transfers(SUB_API_NOTSET, itransfer);
}

static int windows_cancel_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);

	switch (transfer->type) {
	case LIBUSB_TRANSFER_TYPE_CONTROL:
		return windows_abort_control(itransfer);
	case LIBUSB_TRANSFER_TYPE_BULK:
	case LIBUSB_TRANSFER_TYPE_INTERRUPT:
	case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
		return windows_abort_transfers(itransfer);
	case LIBUSB_TRANSFER_TYPE_BULK_STREAM:
		return LIBUSB_ERROR_NOT_SUPPORTED;
	default:
		usbi_err(ITRANSFER_CTX(itransfer), "unknown endpoint type %d", transfer->type);
		return LIBUSB_ERROR_INVALID_PARAM;
	}
}

static void windows_transfer_callback(struct usbi_transfer *itransfer, uint32_t io_result, uint32_t io_size)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	int status, istatus;

	usbi_dbg("handling I/O completion with errcode %d, size %d", io_result, io_size);

	switch(io_result) {
	case NO_ERROR:
		status = priv->apib->copy_transfer_data(SUB_API_NOTSET, itransfer, io_size);
		break;
	case ERROR_GEN_FAILURE:
		usbi_dbg("detected endpoint stall");
		status = LIBUSB_TRANSFER_STALL;
		break;
	case ERROR_SEM_TIMEOUT:
		usbi_dbg("detected semaphore timeout");
		status = LIBUSB_TRANSFER_TIMED_OUT;
		break;
	case ERROR_OPERATION_ABORTED:
		istatus = priv->apib->copy_transfer_data(SUB_API_NOTSET, itransfer, io_size);
		if (istatus != LIBUSB_TRANSFER_COMPLETED) {
			usbi_dbg("Failed to copy partial data in aborted operation: %d", istatus);
		}
		if (itransfer->flags & USBI_TRANSFER_TIMED_OUT) {
			usbi_dbg("detected timeout");
			status = LIBUSB_TRANSFER_TIMED_OUT;
		} else {
			usbi_dbg("detected operation aborted");
			status = LIBUSB_TRANSFER_CANCELLED;
		}
		break;
	default:
		usbi_err(ITRANSFER_CTX(itransfer), "detected I/O error %d: %s", io_result, windows_error_str(io_result));
		status = LIBUSB_TRANSFER_ERROR;
		break;
	}
	windows_clear_transfer_priv(itransfer);	// Cancel polling
	usbi_handle_transfer_completion(itransfer, (enum libusb_transfer_status)status);
}

static void windows_handle_callback (struct usbi_transfer *itransfer, uint32_t io_result, uint32_t io_size)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);

	switch (transfer->type) {
	case LIBUSB_TRANSFER_TYPE_CONTROL:
	case LIBUSB_TRANSFER_TYPE_BULK:
	case LIBUSB_TRANSFER_TYPE_INTERRUPT:
	case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
		windows_transfer_callback (itransfer, io_result, io_size);
		break;
	case LIBUSB_TRANSFER_TYPE_BULK_STREAM:
		usbi_warn(ITRANSFER_CTX(itransfer), "bulk stream transfers are not yet supported on this platform");
		break;
	default:
		usbi_err(ITRANSFER_CTX(itransfer), "unknown endpoint type %d", transfer->type);
	}
}

static int windows_handle_events(struct libusb_context *ctx, struct pollfd *fds, POLL_NFDS_TYPE nfds, int num_ready)
{
	struct windows_transfer_priv *transfer_priv = NULL;
	POLL_NFDS_TYPE i = 0;
	bool found = false;
	struct usbi_transfer *transfer;
	DWORD io_size, io_result;

	usbi_mutex_lock(&ctx->open_devs_lock);
	for (i = 0; i < nfds && num_ready > 0; i++) {

		usbi_dbg("checking fd %d with revents = %04x", fds[i].fd, fds[i].revents);

		if (!fds[i].revents) {
			continue;
		}

		num_ready--;

		// Because a Windows OVERLAPPED is used for poll emulation,
		// a pollable fd is created and stored with each transfer
		usbi_mutex_lock(&ctx->flying_transfers_lock);
		list_for_each_entry(transfer, &ctx->flying_transfers, list, struct usbi_transfer) {
			transfer_priv = usbi_transfer_get_os_priv(transfer);
			if (transfer_priv->pollable_fd.fd == fds[i].fd) {
				found = true;
				break;
			}
		}
		usbi_mutex_unlock(&ctx->flying_transfers_lock);

		if (found) {
			// Handle async requests that completed synchronously first
			if (HasOverlappedIoCompletedSync(transfer_priv->pollable_fd.overlapped)) {
				io_result = NO_ERROR;
				io_size = (DWORD)transfer_priv->pollable_fd.overlapped->InternalHigh;
			// Regular async overlapped
			} else if (GetOverlappedResult(transfer_priv->pollable_fd.handle,
				transfer_priv->pollable_fd.overlapped, &io_size, false)) {
				io_result = NO_ERROR;
			} else {
				io_result = GetLastError();
			}
			usbi_remove_pollfd(ctx, transfer_priv->pollable_fd.fd);
			// let handle_callback free the event using the transfer wfd
			// If you don't use the transfer wfd, you run a risk of trying to free a
			// newly allocated wfd that took the place of the one from the transfer.
			windows_handle_callback(transfer, io_result, io_size);
		} else {
			usbi_err(ctx, "could not find a matching transfer for fd %x", fds[i]);
			return LIBUSB_ERROR_NOT_FOUND;
		}
	}

	usbi_mutex_unlock(&ctx->open_devs_lock);
	return LIBUSB_SUCCESS;
}

/*
 * Monotonic and real time functions
 */
unsigned __stdcall windows_clock_gettime_threaded(void *param)
{
	LARGE_INTEGER hires_counter, li_frequency;
	LONG nb_responses;
	int timer_index;

	// Init - find out if we have access to a monotonic (hires) timer
	if (!QueryPerformanceFrequency(&li_frequency)) {
		usbi_dbg("no hires timer available on this platform");
		hires_frequency = 0;
		hires_ticks_to_ps = UINT64_C(0);
	} else {
		hires_frequency = li_frequency.QuadPart;
		// The hires frequency can go as high as 4 GHz, so we'll use a conversion
		// to picoseconds to compute the tv_nsecs part in clock_gettime
		hires_ticks_to_ps = UINT64_C(1000000000000) / hires_frequency;
		usbi_dbg("hires timer available (Frequency: %"PRIu64" Hz)", hires_frequency);
	}

	// Signal windows_init() that we're ready to service requests
	if (ReleaseSemaphore(timer_response, 1, NULL) == 0) {
		usbi_dbg("unable to release timer semaphore: %s", windows_error_str(0));
	}

	// Main loop - wait for requests
	while (1) {
		timer_index = WaitForMultipleObjects(2, timer_request, FALSE, INFINITE) - WAIT_OBJECT_0;
		if ( (timer_index != 0) && (timer_index != 1) ) {
			usbi_dbg("failure to wait on requests: %s", windows_error_str(0));
			continue;
		}
		if (request_count[timer_index] == 0) {
			// Request already handled
			ResetEvent(timer_request[timer_index]);
			// There's still a possiblity that a thread sends a request between the
			// time we test request_count[] == 0 and we reset the event, in which case
			// the request would be ignored. The simple solution to that is to test
			// request_count again and process requests if non zero.
			if (request_count[timer_index] == 0)
				continue;
		}
		switch (timer_index) {
		case 0:
			WaitForSingleObject(timer_mutex, INFINITE);
			// Requests to this thread are for hires always
			if ((QueryPerformanceCounter(&hires_counter) != 0) && (hires_frequency != 0)) {
				timer_tp.tv_sec = (long)(hires_counter.QuadPart / hires_frequency);
				timer_tp.tv_nsec = (long)(((hires_counter.QuadPart % hires_frequency)/1000) * hires_ticks_to_ps);
			} else {
				// Fallback to real-time if we can't get monotonic value
				// Note that real-time clock does not wait on the mutex or this thread.
				windows_clock_gettime(USBI_CLOCK_REALTIME, &timer_tp);
			}
			ReleaseMutex(timer_mutex);

			nb_responses = InterlockedExchange((LONG*)&request_count[0], 0);
			if ( (nb_responses)
			  && (ReleaseSemaphore(timer_response, nb_responses, NULL) == 0) ) {
				usbi_dbg("unable to release timer semaphore: %s", windows_error_str(0));
			}
			continue;
		case 1: // time to quit
			usbi_dbg("timer thread quitting");
			return 0;
		}
	}
}

static int windows_clock_gettime(int clk_id, struct timespec *tp)
{
	FILETIME filetime;
	ULARGE_INTEGER rtime;
	DWORD r;
	switch(clk_id) {
	case USBI_CLOCK_MONOTONIC:
		if (hires_frequency != 0) {
			while (1) {
				InterlockedIncrement((LONG*)&request_count[0]);
				SetEvent(timer_request[0]);
				r = WaitForSingleObject(timer_response, TIMER_REQUEST_RETRY_MS);
				switch(r) {
				case WAIT_OBJECT_0:
					WaitForSingleObject(timer_mutex, INFINITE);
					*tp = timer_tp;
					ReleaseMutex(timer_mutex);
					return LIBUSB_SUCCESS;
				case WAIT_TIMEOUT:
					usbi_dbg("could not obtain a timer value within reasonable timeframe - too much load?");
					break; // Retry until successful
				default:
					usbi_dbg("WaitForSingleObject failed: %s", windows_error_str(0));
					return LIBUSB_ERROR_OTHER;
				}
			}
		}
		// Fall through and return real-time if monotonic was not detected @ timer init
	case USBI_CLOCK_REALTIME:
		// We follow http://msdn.microsoft.com/en-us/library/ms724928%28VS.85%29.aspx
		// with a predef epoch_time to have an epoch that starts at 1970.01.01 00:00
		// Note however that our resolution is bounded by the Windows system time
		// functions and is at best of the order of 1 ms (or, usually, worse)
		GetSystemTimeAsFileTime(&filetime);
		rtime.LowPart = filetime.dwLowDateTime;
		rtime.HighPart = filetime.dwHighDateTime;
		rtime.QuadPart -= epoch_time;
		tp->tv_sec = (long)(rtime.QuadPart / 10000000);
		tp->tv_nsec = (long)((rtime.QuadPart % 10000000)*100);
		return LIBUSB_SUCCESS;
	default:
		return LIBUSB_ERROR_INVALID_PARAM;
	}
}


// NB: MSVC6 does not support named initializers.
const struct usbi_os_backend windows_backend = {
	"Windows",
	USBI_CAP_HAS_HID_ACCESS,
	windows_init,
	windows_exit,

	NULL, /* windows_get_device_list */
	NULL, /* hotplug_poll */
	windows_open,
	windows_close,

	windows_get_device_descriptor,
	windows_get_active_config_descriptor,
	windows_get_config_descriptor,
	NULL, /* get_config_descriptor_by_value() */

	windows_get_configuration,
	windows_set_configuration,
	windows_claim_interface,
	windows_release_interface,

	windows_set_interface_altsetting,
	windows_clear_halt,
	windows_reset_device,

	NULL,				/* alloc_streams */
	NULL,				/* free_streams */

	windows_kernel_driver_active,
	windows_detach_kernel_driver,
	windows_attach_kernel_driver,

	windows_destroy_device,

	windows_submit_transfer,
	windows_cancel_transfer,
	windows_clear_transfer_priv,

	windows_handle_events,

	windows_clock_gettime,
#if defined(USBI_TIMERFD_AVAILABLE)
	NULL,
#endif
	sizeof(struct windows_device_priv),
	sizeof(struct windows_device_handle_priv),
	sizeof(struct windows_transfer_priv),
	0,
};


/*
 * USB API backends
 */
static int unsupported_init(int sub_api, struct libusb_context *ctx) {
	return LIBUSB_SUCCESS;
}
static int unsupported_exit(int sub_api) {
	return LIBUSB_SUCCESS;
}
static int unsupported_open(int sub_api, struct libusb_device_handle *dev_handle) {
	PRINT_UNSUPPORTED_API(open);
}
static void unsupported_close(int sub_api, struct libusb_device_handle *dev_handle) {
	usbi_dbg("unsupported API call for 'close'");
}
static int unsupported_configure_endpoints(int sub_api, struct libusb_device_handle *dev_handle, int iface) {
	PRINT_UNSUPPORTED_API(configure_endpoints);
}
static int unsupported_claim_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface) {
	PRINT_UNSUPPORTED_API(claim_interface);
}
static int unsupported_set_interface_altsetting(int sub_api, struct libusb_device_handle *dev_handle, int iface, int altsetting) {
	PRINT_UNSUPPORTED_API(set_interface_altsetting);
}
static int unsupported_release_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface) {
	PRINT_UNSUPPORTED_API(release_interface);
}
static int unsupported_clear_halt(int sub_api, struct libusb_device_handle *dev_handle, unsigned char endpoint) {
	PRINT_UNSUPPORTED_API(clear_halt);
}
static int unsupported_reset_device(int sub_api, struct libusb_device_handle *dev_handle) {
	PRINT_UNSUPPORTED_API(reset_device);
}
static int unsupported_submit_bulk_transfer(int sub_api, struct usbi_transfer *itransfer) {
	PRINT_UNSUPPORTED_API(submit_bulk_transfer);
}
static int unsupported_submit_iso_transfer(int sub_api, struct usbi_transfer *itransfer) {
	PRINT_UNSUPPORTED_API(submit_iso_transfer);
}
static int unsupported_submit_control_transfer(int sub_api, struct usbi_transfer *itransfer) {
	PRINT_UNSUPPORTED_API(submit_control_transfer);
}
static int unsupported_abort_control(int sub_api, struct usbi_transfer *itransfer) {
	PRINT_UNSUPPORTED_API(abort_control);
}
static int unsupported_abort_transfers(int sub_api, struct usbi_transfer *itransfer) {
	PRINT_UNSUPPORTED_API(abort_transfers);
}
static int unsupported_copy_transfer_data(int sub_api, struct usbi_transfer *itransfer, uint32_t io_size) {
	PRINT_UNSUPPORTED_API(copy_transfer_data);
}
static int common_configure_endpoints(int sub_api, struct libusb_device_handle *dev_handle, int iface) {
	return LIBUSB_SUCCESS;
}
// These names must be uppercase
const char* hub_driver_names[] = {"USBHUB", "USBHUB3", "USB3HUB", "NUSB3HUB", "RUSB3HUB", "FLXHCIH", "TIHUB3", "ETRONHUB3", "VIAHUB3", "ASMTHUB3", "IUSB3HUB", "VUSB3HUB", "AMDHUB30"};
const char* composite_driver_names[] = {"USBCCGP"};
const char* winusbx_driver_names[] = WINUSBX_DRV_NAMES;
const char* hid_driver_names[] = {"HIDUSB", "MOUHID", "KBDHID"};
const struct windows_usb_api_backend usb_api_backend[USB_API_MAX] = {
	{
		USB_API_UNSUPPORTED,
		"Unsupported API",
		NULL,
		0,
		unsupported_init,
		unsupported_exit,
		unsupported_open,
		unsupported_close,
		unsupported_configure_endpoints,
		unsupported_claim_interface,
		unsupported_set_interface_altsetting,
		unsupported_release_interface,
		unsupported_clear_halt,
		unsupported_reset_device,
		unsupported_submit_bulk_transfer,
		unsupported_submit_iso_transfer,
		unsupported_submit_control_transfer,
		unsupported_abort_control,
		unsupported_abort_transfers,
		unsupported_copy_transfer_data,
	}, {
		USB_API_HUB,
		"HUB API",
		hub_driver_names,
		ARRAYSIZE(hub_driver_names),
		unsupported_init,
		unsupported_exit,
		unsupported_open,
		unsupported_close,
		unsupported_configure_endpoints,
		unsupported_claim_interface,
		unsupported_set_interface_altsetting,
		unsupported_release_interface,
		unsupported_clear_halt,
		unsupported_reset_device,
		unsupported_submit_bulk_transfer,
		unsupported_submit_iso_transfer,
		unsupported_submit_control_transfer,
		unsupported_abort_control,
		unsupported_abort_transfers,
		unsupported_copy_transfer_data,
	}, {
		USB_API_COMPOSITE,
		"Composite API",
		composite_driver_names,
		ARRAYSIZE(composite_driver_names),
		composite_init,
		composite_exit,
		composite_open,
		composite_close,
		common_configure_endpoints,
		composite_claim_interface,
		composite_set_interface_altsetting,
		composite_release_interface,
		composite_clear_halt,
		composite_reset_device,
		composite_submit_bulk_transfer,
		composite_submit_iso_transfer,
		composite_submit_control_transfer,
		composite_abort_control,
		composite_abort_transfers,
		composite_copy_transfer_data,
	}, {
		USB_API_WINUSBX,
		"WinUSB-like APIs",
		winusbx_driver_names,
		ARRAYSIZE(winusbx_driver_names),
		winusbx_init,
		winusbx_exit,
		winusbx_open,
		winusbx_close,
		winusbx_configure_endpoints,
		winusbx_claim_interface,
		winusbx_set_interface_altsetting,
		winusbx_release_interface,
		winusbx_clear_halt,
		winusbx_reset_device,
		winusbx_submit_bulk_transfer,
		unsupported_submit_iso_transfer,
		winusbx_submit_control_transfer,
		winusbx_abort_control,
		winusbx_abort_transfers,
		winusbx_copy_transfer_data,
	}, {
		USB_API_HID,
		"HID API",
		hid_driver_names,
		ARRAYSIZE(hid_driver_names),
		hid_init,
		hid_exit,
		hid_open,
		hid_close,
		common_configure_endpoints,
		hid_claim_interface,
		hid_set_interface_altsetting,
		hid_release_interface,
		hid_clear_halt,
		hid_reset_device,
		hid_submit_bulk_transfer,
		unsupported_submit_iso_transfer,
		hid_submit_control_transfer,
		hid_abort_transfers,
		hid_abort_transfers,
		hid_copy_transfer_data,
	},
};


/*
 * WinUSB-like (WinUSB, libusb0/libusbK through libusbk DLL) API functions
 */
#define WinUSBX_Set(fn) do { if (native_winusb) WinUSBX[i].fn = (WinUsb_##fn##_t) GetProcAddress(h, "WinUsb_" #fn); \
	else pLibK_GetProcAddress((PVOID*)&WinUSBX[i].fn, i, KUSB_FNID_##fn); } while (0)

static int winusbx_init(int sub_api, struct libusb_context *ctx)
{
	HMODULE h = NULL;
	bool native_winusb = false;
	int i;
	KLIB_VERSION LibK_Version;
	LibK_GetProcAddress_t pLibK_GetProcAddress = NULL;
	LibK_GetVersion_t pLibK_GetVersion = NULL;

	h = GetModuleHandleA("libusbK");
	if (h == NULL) {
		h = LoadLibraryA("libusbK");
	}
	if (h == NULL) {
		usbi_info(ctx, "libusbK DLL is not available, will use native WinUSB");
		h = GetModuleHandleA("WinUSB");
		if (h == NULL) {
			h = LoadLibraryA("WinUSB");
		}		if (h == NULL) {
			usbi_warn(ctx, "WinUSB DLL is not available either,\n"
				"you will not be able to access devices outside of enumeration");
			return LIBUSB_ERROR_NOT_FOUND;
		}
	} else {
		usbi_dbg("using libusbK DLL for universal access");
		pLibK_GetVersion = (LibK_GetVersion_t) GetProcAddress(h, "LibK_GetVersion");
		if (pLibK_GetVersion != NULL) {
			pLibK_GetVersion(&LibK_Version);
			usbi_dbg("libusbK version: %d.%d.%d.%d", LibK_Version.Major, LibK_Version.Minor,
				LibK_Version.Micro, LibK_Version.Nano);
		}
		pLibK_GetProcAddress = (LibK_GetProcAddress_t) GetProcAddress(h, "LibK_GetProcAddress");
		if (pLibK_GetProcAddress == NULL) {
			usbi_err(ctx, "LibK_GetProcAddress() not found in libusbK DLL");
			return LIBUSB_ERROR_NOT_FOUND;
		}
	}
	native_winusb = (pLibK_GetProcAddress == NULL);
	for (i=SUB_API_LIBUSBK; i<SUB_API_MAX; i++) {
		WinUSBX_Set(AbortPipe);
		WinUSBX_Set(ControlTransfer);
		WinUSBX_Set(FlushPipe);
		WinUSBX_Set(Free);
		WinUSBX_Set(GetAssociatedInterface);
		WinUSBX_Set(GetCurrentAlternateSetting);
		WinUSBX_Set(GetDescriptor);
		WinUSBX_Set(GetOverlappedResult);
		WinUSBX_Set(GetPipePolicy);
		WinUSBX_Set(GetPowerPolicy);
		WinUSBX_Set(Initialize);
		WinUSBX_Set(QueryDeviceInformation);
		WinUSBX_Set(QueryInterfaceSettings);
		WinUSBX_Set(QueryPipe);
		WinUSBX_Set(ReadPipe);
		WinUSBX_Set(ResetPipe);
		WinUSBX_Set(SetCurrentAlternateSetting);
		WinUSBX_Set(SetPipePolicy);
		WinUSBX_Set(SetPowerPolicy);
		WinUSBX_Set(WritePipe);
		if (!native_winusb) {
			WinUSBX_Set(ResetDevice);
		}
		if (WinUSBX[i].Initialize != NULL) {
			WinUSBX[i].initialized = true;
			usbi_dbg("initalized sub API %s", sub_api_name[i]);
		} else {
			usbi_warn(ctx, "Failed to initalize sub API %s", sub_api_name[i]);
			WinUSBX[i].initialized = false;
		}
	}
	return LIBUSB_SUCCESS;
}

static int winusbx_exit(int sub_api)
{
	return LIBUSB_SUCCESS;
}

// NB: open and close must ensure that they only handle interface of
// the right API type, as these functions can be called wholesale from
// composite_open(), with interfaces belonging to different APIs
static int winusbx_open(int sub_api, struct libusb_device_handle *dev_handle)
{
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);

	HANDLE file_handle;
	int i;

	CHECK_WINUSBX_AVAILABLE(sub_api);

	// WinUSB requires a seperate handle for each interface
	for (i = 0; i < USB_MAXINTERFACES; i++) {
		if ( (priv->usb_interface[i].path != NULL)
		  && (priv->usb_interface[i].apib->id == USB_API_WINUSBX) ) {
			file_handle = CreateFileA(priv->usb_interface[i].path, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ,
				NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
			if (file_handle == INVALID_HANDLE_VALUE) {
				usbi_err(ctx, "could not open device %s (interface %d): %s", priv->usb_interface[i].path, i, windows_error_str(0));
				switch(GetLastError()) {
				case ERROR_FILE_NOT_FOUND:	// The device was disconnected
					return LIBUSB_ERROR_NO_DEVICE;
				case ERROR_ACCESS_DENIED:
					return LIBUSB_ERROR_ACCESS;
				default:
					return LIBUSB_ERROR_IO;
				}
			}
			handle_priv->interface_handle[i].dev_handle = file_handle;
		}
	}

	return LIBUSB_SUCCESS;
}

static void winusbx_close(int sub_api, struct libusb_device_handle *dev_handle)
{
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	HANDLE file_handle;
	int i;

	if (sub_api == SUB_API_NOTSET)
		sub_api = priv->sub_api;
	if (!WinUSBX[sub_api].initialized)
		return;

	for (i = 0; i < USB_MAXINTERFACES; i++) {
		if (priv->usb_interface[i].apib->id == USB_API_WINUSBX) {
			file_handle = handle_priv->interface_handle[i].dev_handle;
			if ( (file_handle != 0) && (file_handle != INVALID_HANDLE_VALUE)) {
				CloseHandle(file_handle);
			}
		}
	}
}

static int winusbx_configure_endpoints(int sub_api, struct libusb_device_handle *dev_handle, int iface)
{
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	HANDLE winusb_handle = handle_priv->interface_handle[iface].api_handle;
	UCHAR policy;
	ULONG timeout = 0;
	uint8_t endpoint_address;
	int i;

	CHECK_WINUSBX_AVAILABLE(sub_api);

	// With handle and enpoints set (in parent), we can setup the default pipe properties
	// see http://download.microsoft.com/download/D/1/D/D1DD7745-426B-4CC3-A269-ABBBE427C0EF/DVC-T705_DDC08.pptx
	for (i=-1; i<priv->usb_interface[iface].nb_endpoints; i++) {
		endpoint_address =(i==-1)?0:priv->usb_interface[iface].endpoint[i];
		if (!WinUSBX[sub_api].SetPipePolicy(winusb_handle, endpoint_address,
			PIPE_TRANSFER_TIMEOUT, sizeof(ULONG), &timeout)) {
			usbi_dbg("failed to set PIPE_TRANSFER_TIMEOUT for control endpoint %02X", endpoint_address);
		}
		if ((i == -1) || (sub_api == SUB_API_LIBUSB0)) {
			continue;	// Other policies don't apply to control endpoint or libusb0
		}
		policy = false;
		if (!WinUSBX[sub_api].SetPipePolicy(winusb_handle, endpoint_address,
			SHORT_PACKET_TERMINATE, sizeof(UCHAR), &policy)) {
			usbi_dbg("failed to disable SHORT_PACKET_TERMINATE for endpoint %02X", endpoint_address);
		}
		if (!WinUSBX[sub_api].SetPipePolicy(winusb_handle, endpoint_address,
			IGNORE_SHORT_PACKETS, sizeof(UCHAR), &policy)) {
			usbi_dbg("failed to disable IGNORE_SHORT_PACKETS for endpoint %02X", endpoint_address);
		}
		policy = true;
		/* ALLOW_PARTIAL_READS must be enabled due to likely libusbK bug. See:
		   https://sourceforge.net/mailarchive/message.php?msg_id=29736015 */
		if (!WinUSBX[sub_api].SetPipePolicy(winusb_handle, endpoint_address,
			ALLOW_PARTIAL_READS, sizeof(UCHAR), &policy)) {
			usbi_dbg("failed to enable ALLOW_PARTIAL_READS for endpoint %02X", endpoint_address);
		}
		if (!WinUSBX[sub_api].SetPipePolicy(winusb_handle, endpoint_address,
			AUTO_CLEAR_STALL, sizeof(UCHAR), &policy)) {
			usbi_dbg("failed to enable AUTO_CLEAR_STALL for endpoint %02X", endpoint_address);
		}
	}

	return LIBUSB_SUCCESS;
}

static int winusbx_claim_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface)
{
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	bool is_using_usbccgp = (priv->apib->id == USB_API_COMPOSITE);
	HANDLE file_handle, winusb_handle;
	DWORD err;
	int i;
	SP_DEVICE_INTERFACE_DETAIL_DATA_A *dev_interface_details = NULL;
	HDEVINFO dev_info = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA dev_info_data;
	char *dev_path_no_guid = NULL;
	char filter_path[] = "\\\\.\\libusb0-0000";
	bool found_filter = false;

	CHECK_WINUSBX_AVAILABLE(sub_api);

	// If the device is composite, but using the default Windows composite parent driver (usbccgp)
	// or if it's the first WinUSB-like interface, we get a handle through Initialize().
	if ((is_using_usbccgp) || (iface == 0)) {
		// composite device (independent interfaces) or interface 0
		file_handle = handle_priv->interface_handle[iface].dev_handle;
		if ((file_handle == 0) || (file_handle == INVALID_HANDLE_VALUE)) {
			return LIBUSB_ERROR_NOT_FOUND;
		}

		if (!WinUSBX[sub_api].Initialize(file_handle, &winusb_handle)) {
			handle_priv->interface_handle[iface].api_handle = INVALID_HANDLE_VALUE;
			err = GetLastError();
			switch(err) {
			case ERROR_BAD_COMMAND:
				// The device was disconnected
				usbi_err(ctx, "could not access interface %d: %s", iface, windows_error_str(0));
				return LIBUSB_ERROR_NO_DEVICE;
			default:
				// it may be that we're using the libusb0 filter driver.
				// TODO: can we move this whole business into the K/0 DLL?
				for (i = 0; ; i++) {
					safe_free(dev_interface_details);
					safe_free(dev_path_no_guid);
					dev_interface_details = get_interface_details_filter(ctx, &dev_info, &dev_info_data, &GUID_DEVINTERFACE_LIBUSB0_FILTER, i, filter_path);
					if ((found_filter) || (dev_interface_details == NULL)) {
						break;
					}
					// ignore GUID part
					dev_path_no_guid = sanitize_path(strtok(dev_interface_details->DevicePath, "{"));
					if (safe_strncmp(dev_path_no_guid, priv->usb_interface[iface].path, safe_strlen(dev_path_no_guid)) == 0) {
						file_handle = CreateFileA(filter_path, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ,
							NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
						if (file_handle == INVALID_HANDLE_VALUE) {
							usbi_err(ctx, "could not open device %s: %s", filter_path, windows_error_str(0));
						} else {
							WinUSBX[sub_api].Free(winusb_handle);
							if (WinUSBX[sub_api].Initialize(file_handle, &winusb_handle))
								found_filter = true;
							else
								usbi_err(ctx, "could not initialize filter driver for %s", filter_path);
						}
					}
				}
				if (!found_filter) {
					usbi_err(ctx, "could not access interface %d: %s", iface, windows_error_str(err));
					return LIBUSB_ERROR_ACCESS;
				}
			}
		}
		handle_priv->interface_handle[iface].api_handle = winusb_handle;
	} else {
		// For all other interfaces, use GetAssociatedInterface()
		winusb_handle = handle_priv->interface_handle[0].api_handle;
		// It is a requirement for multiple interface devices on Windows that, to you
		// must first claim the first interface before you claim the others
		if ((winusb_handle == 0) || (winusb_handle == INVALID_HANDLE_VALUE)) {
			file_handle = handle_priv->interface_handle[0].dev_handle;
			if (WinUSBX[sub_api].Initialize(file_handle, &winusb_handle)) {
				handle_priv->interface_handle[0].api_handle = winusb_handle;
				usbi_warn(ctx, "auto-claimed interface 0 (required to claim %d with WinUSB)", iface);
			} else {
				usbi_warn(ctx, "failed to auto-claim interface 0 (required to claim %d with WinUSB): %s", iface, windows_error_str(0));
				return LIBUSB_ERROR_ACCESS;
			}
		}
		if (!WinUSBX[sub_api].GetAssociatedInterface(winusb_handle, (UCHAR)(iface-1),
			&handle_priv->interface_handle[iface].api_handle)) {
			handle_priv->interface_handle[iface].api_handle = INVALID_HANDLE_VALUE;
			switch(GetLastError()) {
			case ERROR_NO_MORE_ITEMS:   // invalid iface
				return LIBUSB_ERROR_NOT_FOUND;
			case ERROR_BAD_COMMAND:     // The device was disconnected
				return LIBUSB_ERROR_NO_DEVICE;
			case ERROR_ALREADY_EXISTS:  // already claimed
				return LIBUSB_ERROR_BUSY;
			default:
				usbi_err(ctx, "could not claim interface %d: %s", iface, windows_error_str(0));
				return LIBUSB_ERROR_ACCESS;
			}
		}
	}
	usbi_dbg("claimed interface %d", iface);
	handle_priv->active_interface = iface;

	return LIBUSB_SUCCESS;
}

static int winusbx_release_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface)
{
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	HANDLE winusb_handle;

	CHECK_WINUSBX_AVAILABLE(sub_api);

	winusb_handle = handle_priv->interface_handle[iface].api_handle;
	if ((winusb_handle == 0) || (winusb_handle == INVALID_HANDLE_VALUE)) {
		return LIBUSB_ERROR_NOT_FOUND;
	}

	WinUSBX[sub_api].Free(winusb_handle);
	handle_priv->interface_handle[iface].api_handle = INVALID_HANDLE_VALUE;

	return LIBUSB_SUCCESS;
}

/*
 * Return the first valid interface (of the same API type), for control transfers
 */
static int get_valid_interface(struct libusb_device_handle *dev_handle, int api_id)
{
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	int i;

	if ((api_id < USB_API_WINUSBX) || (api_id > USB_API_HID)) {
		usbi_dbg("unsupported API ID");
		return -1;
	}

	for (i=0; i<USB_MAXINTERFACES; i++) {
		if ( (handle_priv->interface_handle[i].dev_handle != 0)
		  && (handle_priv->interface_handle[i].dev_handle != INVALID_HANDLE_VALUE)
		  && (handle_priv->interface_handle[i].api_handle != 0)
		  && (handle_priv->interface_handle[i].api_handle != INVALID_HANDLE_VALUE)
		  && (priv->usb_interface[i].apib->id == api_id) ) {
			return i;
		}
	}
	return -1;
}

/*
 * Lookup interface by endpoint address. -1 if not found
 */
static int interface_by_endpoint(struct windows_device_priv *priv,
	struct windows_device_handle_priv *handle_priv, uint8_t endpoint_address)
{
	int i, j;
	for (i=0; i<USB_MAXINTERFACES; i++) {
		if (handle_priv->interface_handle[i].api_handle == INVALID_HANDLE_VALUE)
			continue;
		if (handle_priv->interface_handle[i].api_handle == 0)
			continue;
		if (priv->usb_interface[i].endpoint == NULL)
			continue;
		for (j=0; j<priv->usb_interface[i].nb_endpoints; j++) {
			if (priv->usb_interface[i].endpoint[j] == endpoint_address) {
				return i;
			}
		}
	}
	return -1;
}

static int winusbx_submit_control_transfer(int sub_api, struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(
		transfer->dev_handle);
	WINUSB_SETUP_PACKET *setup = (WINUSB_SETUP_PACKET *) transfer->buffer;
	ULONG size;
	HANDLE winusb_handle;
	int current_interface;
	struct winfd wfd;

	CHECK_WINUSBX_AVAILABLE(sub_api);

	transfer_priv->pollable_fd = INVALID_WINFD;
	size = transfer->length - LIBUSB_CONTROL_SETUP_SIZE;

	if (size > MAX_CTRL_BUFFER_LENGTH)
		return LIBUSB_ERROR_INVALID_PARAM;

	current_interface = get_valid_interface(transfer->dev_handle, USB_API_WINUSBX);
	if (current_interface < 0) {
		if (auto_claim(transfer, &current_interface, USB_API_WINUSBX) != LIBUSB_SUCCESS) {
			return LIBUSB_ERROR_NOT_FOUND;
		}
	}

	usbi_dbg("will use interface %d", current_interface);
	winusb_handle = handle_priv->interface_handle[current_interface].api_handle;

	wfd = usbi_create_fd(winusb_handle, RW_READ, NULL, NULL);
	// Always use the handle returned from usbi_create_fd (wfd.handle)
	if (wfd.fd < 0) {
		return LIBUSB_ERROR_NO_MEM;
	}

	// Sending of set configuration control requests from WinUSB creates issues
	if ( ((setup->request_type & (0x03 << 5)) == LIBUSB_REQUEST_TYPE_STANDARD)
	  && (setup->request == LIBUSB_REQUEST_SET_CONFIGURATION) ) {
		if (setup->value != priv->active_config) {
			usbi_warn(ctx, "cannot set configuration other than the default one");
			usbi_free_fd(&wfd);
			return LIBUSB_ERROR_INVALID_PARAM;
		}
		wfd.overlapped->Internal = STATUS_COMPLETED_SYNCHRONOUSLY;
		wfd.overlapped->InternalHigh = 0;
	} else {
		if (!WinUSBX[sub_api].ControlTransfer(wfd.handle, *setup, transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE, size, NULL, wfd.overlapped)) {
			if(GetLastError() != ERROR_IO_PENDING) {
				usbi_warn(ctx, "ControlTransfer failed: %s", windows_error_str(0));
				usbi_free_fd(&wfd);
				return LIBUSB_ERROR_IO;
			}
		} else {
			wfd.overlapped->Internal = STATUS_COMPLETED_SYNCHRONOUSLY;
			wfd.overlapped->InternalHigh = (DWORD)size;
		}
	}

	// Use priv_transfer to store data needed for async polling
	transfer_priv->pollable_fd = wfd;
	transfer_priv->interface_number = (uint8_t)current_interface;

	return LIBUSB_SUCCESS;
}

static int winusbx_set_interface_altsetting(int sub_api, struct libusb_device_handle *dev_handle, int iface, int altsetting)
{
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	HANDLE winusb_handle;

	CHECK_WINUSBX_AVAILABLE(sub_api);

	if (altsetting > 255) {
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	winusb_handle = handle_priv->interface_handle[iface].api_handle;
	if ((winusb_handle == 0) || (winusb_handle == INVALID_HANDLE_VALUE)) {
		usbi_err(ctx, "interface must be claimed first");
		return LIBUSB_ERROR_NOT_FOUND;
	}

	if (!WinUSBX[sub_api].SetCurrentAlternateSetting(winusb_handle, (UCHAR)altsetting)) {
		usbi_err(ctx, "SetCurrentAlternateSetting failed: %s", windows_error_str(0));
		return LIBUSB_ERROR_IO;
	}

	return LIBUSB_SUCCESS;
}

static int winusbx_submit_bulk_transfer(int sub_api, struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(transfer->dev_handle);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	HANDLE winusb_handle;
	bool ret;
	int current_interface;
	struct winfd wfd;

	CHECK_WINUSBX_AVAILABLE(sub_api);

	transfer_priv->pollable_fd = INVALID_WINFD;

	current_interface = interface_by_endpoint(priv, handle_priv, transfer->endpoint);
	if (current_interface < 0) {
		usbi_err(ctx, "unable to match endpoint to an open interface - cancelling transfer");
		return LIBUSB_ERROR_NOT_FOUND;
	}

	usbi_dbg("matched endpoint %02X with interface %d", transfer->endpoint, current_interface);

	winusb_handle = handle_priv->interface_handle[current_interface].api_handle;

	wfd = usbi_create_fd(winusb_handle, IS_XFERIN(transfer) ? RW_READ : RW_WRITE, NULL, NULL);
	// Always use the handle returned from usbi_create_fd (wfd.handle)
	if (wfd.fd < 0) {
		return LIBUSB_ERROR_NO_MEM;
	}

	if (IS_XFERIN(transfer)) {
		usbi_dbg("reading %d bytes", transfer->length);
		ret = WinUSBX[sub_api].ReadPipe(wfd.handle, transfer->endpoint, transfer->buffer, transfer->length, NULL, wfd.overlapped);
	} else {
		usbi_dbg("writing %d bytes", transfer->length);
		ret = WinUSBX[sub_api].WritePipe(wfd.handle, transfer->endpoint, transfer->buffer, transfer->length, NULL, wfd.overlapped);
	}
	if (!ret) {
		if(GetLastError() != ERROR_IO_PENDING) {
			usbi_err(ctx, "ReadPipe/WritePipe failed: %s", windows_error_str(0));
			usbi_free_fd(&wfd);
			return LIBUSB_ERROR_IO;
		}
	} else {
		wfd.overlapped->Internal = STATUS_COMPLETED_SYNCHRONOUSLY;
		wfd.overlapped->InternalHigh = (DWORD)transfer->length;
	}

	transfer_priv->pollable_fd = wfd;
	transfer_priv->interface_number = (uint8_t)current_interface;

	return LIBUSB_SUCCESS;
}

static int winusbx_clear_halt(int sub_api, struct libusb_device_handle *dev_handle, unsigned char endpoint)
{
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	HANDLE winusb_handle;
	int current_interface;

	CHECK_WINUSBX_AVAILABLE(sub_api);

	current_interface = interface_by_endpoint(priv, handle_priv, endpoint);
	if (current_interface < 0) {
		usbi_err(ctx, "unable to match endpoint to an open interface - cannot clear");
		return LIBUSB_ERROR_NOT_FOUND;
	}

	usbi_dbg("matched endpoint %02X with interface %d", endpoint, current_interface);
	winusb_handle = handle_priv->interface_handle[current_interface].api_handle;

	if (!WinUSBX[sub_api].ResetPipe(winusb_handle, endpoint)) {
		usbi_err(ctx, "ResetPipe failed: %s", windows_error_str(0));
		return LIBUSB_ERROR_NO_DEVICE;
	}

	return LIBUSB_SUCCESS;
}

/*
 * from http://www.winvistatips.com/winusb-bugchecks-t335323.html (confirmed
 * through testing as well):
 * "You can not call WinUsb_AbortPipe on control pipe. You can possibly cancel
 * the control transfer using CancelIo"
 */
static int winusbx_abort_control(int sub_api, struct usbi_transfer *itransfer)
{
	// Cancelling of the I/O is done in the parent
	return LIBUSB_SUCCESS;
}

static int winusbx_abort_transfers(int sub_api, struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(transfer->dev_handle);
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	HANDLE winusb_handle;
	int current_interface;

	CHECK_WINUSBX_AVAILABLE(sub_api);

	current_interface = transfer_priv->interface_number;
	if ((current_interface < 0) || (current_interface >= USB_MAXINTERFACES)) {
		usbi_err(ctx, "program assertion failed: invalid interface_number");
		return LIBUSB_ERROR_NOT_FOUND;
	}
	usbi_dbg("will use interface %d", current_interface);

	winusb_handle = handle_priv->interface_handle[current_interface].api_handle;

	if (!WinUSBX[sub_api].AbortPipe(winusb_handle, transfer->endpoint)) {
		usbi_err(ctx, "AbortPipe failed: %s", windows_error_str(0));
		return LIBUSB_ERROR_NO_DEVICE;
	}

	return LIBUSB_SUCCESS;
}

/*
 * from the "How to Use WinUSB to Communicate with a USB Device" Microsoft white paper
 * (http://www.microsoft.com/whdc/connect/usb/winusb_howto.mspx):
 * "WinUSB does not support host-initiated reset port and cycle port operations" and
 * IOCTL_INTERNAL_USB_CYCLE_PORT is only available in kernel mode and the
 * IOCTL_USB_HUB_CYCLE_PORT ioctl was removed from Vista => the best we can do is
 * cycle the pipes (and even then, the control pipe can not be reset using WinUSB)
 */
// TODO: (post hotplug): see if we can force eject the device and redetect it (reuse hotplug?)
static int winusbx_reset_device(int sub_api, struct libusb_device_handle *dev_handle)
{
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	struct winfd wfd;
	HANDLE winusb_handle;
	int i, j;

	CHECK_WINUSBX_AVAILABLE(sub_api);

	// Reset any available pipe (except control)
	for (i=0; i<USB_MAXINTERFACES; i++) {
		winusb_handle = handle_priv->interface_handle[i].api_handle;
		for (wfd = handle_to_winfd(winusb_handle); wfd.fd > 0;)
		{
			// Cancel any pollable I/O
			usbi_remove_pollfd(ctx, wfd.fd);
			usbi_free_fd(&wfd);
			wfd = handle_to_winfd(winusb_handle);
		}

		if ( (winusb_handle != 0) && (winusb_handle != INVALID_HANDLE_VALUE)) {
			for (j=0; j<priv->usb_interface[i].nb_endpoints; j++) {
				usbi_dbg("resetting ep %02X", priv->usb_interface[i].endpoint[j]);
				if (!WinUSBX[sub_api].AbortPipe(winusb_handle, priv->usb_interface[i].endpoint[j])) {
					usbi_err(ctx, "AbortPipe (pipe address %02X) failed: %s",
						priv->usb_interface[i].endpoint[j], windows_error_str(0));
				}
				// FlushPipe seems to fail on OUT pipes
				if (IS_EPIN(priv->usb_interface[i].endpoint[j])
				  && (!WinUSBX[sub_api].FlushPipe(winusb_handle, priv->usb_interface[i].endpoint[j])) ) {
					usbi_err(ctx, "FlushPipe (pipe address %02X) failed: %s",
						priv->usb_interface[i].endpoint[j], windows_error_str(0));
				}
				if (!WinUSBX[sub_api].ResetPipe(winusb_handle, priv->usb_interface[i].endpoint[j])) {
					usbi_err(ctx, "ResetPipe (pipe address %02X) failed: %s",
						priv->usb_interface[i].endpoint[j], windows_error_str(0));
				}
			}
		}
	}

	// libusbK & libusb0 have the ability to issue an actual device reset
	if (WinUSBX[sub_api].ResetDevice != NULL) {
		winusb_handle = handle_priv->interface_handle[0].api_handle;
		if ( (winusb_handle != 0) && (winusb_handle != INVALID_HANDLE_VALUE)) {
			WinUSBX[sub_api].ResetDevice(winusb_handle);
		}
	}
	return LIBUSB_SUCCESS;
}

static int winusbx_copy_transfer_data(int sub_api, struct usbi_transfer *itransfer, uint32_t io_size)
{
	itransfer->transferred += io_size;
	return LIBUSB_TRANSFER_COMPLETED;
}

/*
 * Internal HID Support functions (from libusb-win32)
 * Note that functions that complete data transfer synchronously must return
 * LIBUSB_COMPLETED instead of LIBUSB_SUCCESS
 */
static int _hid_get_hid_descriptor(struct hid_device_priv *dev, void *data, size_t *size);
static int _hid_get_report_descriptor(struct hid_device_priv *dev, void *data, size_t *size);

static int _hid_wcslen(WCHAR *str)
{
	int i = 0;
	while (str[i] && (str[i] != 0x409)) {
		i++;
	}
	return i;
}

static int _hid_get_device_descriptor(struct hid_device_priv *dev, void *data, size_t *size)
{
	struct libusb_device_descriptor d;

	d.bLength = LIBUSB_DT_DEVICE_SIZE;
	d.bDescriptorType = LIBUSB_DT_DEVICE;
	d.bcdUSB = 0x0200; /* 2.00 */
	d.bDeviceClass = 0;
	d.bDeviceSubClass = 0;
	d.bDeviceProtocol = 0;
	d.bMaxPacketSize0 = 64; /* fix this! */
	d.idVendor = (uint16_t)dev->vid;
	d.idProduct = (uint16_t)dev->pid;
	d.bcdDevice = 0x0100;
	d.iManufacturer = dev->string_index[0];
	d.iProduct = dev->string_index[1];
	d.iSerialNumber = dev->string_index[2];
	d.bNumConfigurations = 1;

	if (*size > LIBUSB_DT_DEVICE_SIZE)
		*size = LIBUSB_DT_DEVICE_SIZE;
	memcpy(data, &d, *size);
	return LIBUSB_COMPLETED;
}

static int _hid_get_config_descriptor(struct hid_device_priv *dev, void *data, size_t *size)
{
	char num_endpoints = 0;
	size_t config_total_len = 0;
	char tmp[HID_MAX_CONFIG_DESC_SIZE];
	struct libusb_config_descriptor *cd;
	struct libusb_interface_descriptor *id;
	struct libusb_hid_descriptor *hd;
	struct libusb_endpoint_descriptor *ed;
	size_t tmp_size;

	if (dev->input_report_size)
		num_endpoints++;
	if (dev->output_report_size)
		num_endpoints++;

	config_total_len = LIBUSB_DT_CONFIG_SIZE + LIBUSB_DT_INTERFACE_SIZE
		+ LIBUSB_DT_HID_SIZE + num_endpoints * LIBUSB_DT_ENDPOINT_SIZE;


	cd = (struct libusb_config_descriptor *)tmp;
	id = (struct libusb_interface_descriptor *)(tmp + LIBUSB_DT_CONFIG_SIZE);
	hd = (struct libusb_hid_descriptor *)(tmp + LIBUSB_DT_CONFIG_SIZE
		+ LIBUSB_DT_INTERFACE_SIZE);
	ed = (struct libusb_endpoint_descriptor *)(tmp + LIBUSB_DT_CONFIG_SIZE
		+ LIBUSB_DT_INTERFACE_SIZE
		+ LIBUSB_DT_HID_SIZE);

	cd->bLength = LIBUSB_DT_CONFIG_SIZE;
	cd->bDescriptorType = LIBUSB_DT_CONFIG;
	cd->wTotalLength = (uint16_t) config_total_len;
	cd->bNumInterfaces = 1;
	cd->bConfigurationValue = 1;
	cd->iConfiguration = 0;
	cd->bmAttributes = 1 << 7; /* bus powered */
	cd->MaxPower = 50;

	id->bLength = LIBUSB_DT_INTERFACE_SIZE;
	id->bDescriptorType = LIBUSB_DT_INTERFACE;
	id->bInterfaceNumber = 0;
	id->bAlternateSetting = 0;
	id->bNumEndpoints = num_endpoints;
	id->bInterfaceClass = 3;
	id->bInterfaceSubClass = 0;
	id->bInterfaceProtocol = 0;
	id->iInterface = 0;

	tmp_size = LIBUSB_DT_HID_SIZE;
	_hid_get_hid_descriptor(dev, hd, &tmp_size);

	if (dev->input_report_size) {
		ed->bLength = LIBUSB_DT_ENDPOINT_SIZE;
		ed->bDescriptorType = LIBUSB_DT_ENDPOINT;
		ed->bEndpointAddress = HID_IN_EP;
		ed->bmAttributes = 3;
		ed->wMaxPacketSize = dev->input_report_size - 1;
		ed->bInterval = 10;
		ed = (struct libusb_endpoint_descriptor *)((char*)ed + LIBUSB_DT_ENDPOINT_SIZE);
	}

	if (dev->output_report_size) {
		ed->bLength = LIBUSB_DT_ENDPOINT_SIZE;
		ed->bDescriptorType = LIBUSB_DT_ENDPOINT;
		ed->bEndpointAddress = HID_OUT_EP;
		ed->bmAttributes = 3;
		ed->wMaxPacketSize = dev->output_report_size - 1;
		ed->bInterval = 10;
	}

	if (*size > config_total_len)
		*size = config_total_len;
	memcpy(data, tmp, *size);
	return LIBUSB_COMPLETED;
}

static int _hid_get_string_descriptor(struct hid_device_priv *dev, int _index,
									  void *data, size_t *size)
{
	void *tmp = NULL;
	size_t tmp_size = 0;
	int i;

	/* language ID, EN-US */
	char string_langid[] = {
		0x09,
		0x04
	};

	if ((*size < 2) || (*size > 255)) {
		return LIBUSB_ERROR_OVERFLOW;
	}

	if (_index == 0) {
		tmp = string_langid;
		tmp_size = sizeof(string_langid)+2;
	} else {
		for (i=0; i<3; i++) {
			if (_index == (dev->string_index[i])) {
				tmp = dev->string[i];
				tmp_size = (_hid_wcslen(dev->string[i])+1) * sizeof(WCHAR);
				break;
			}
		}
		if (i == 3) {	// not found
			return LIBUSB_ERROR_INVALID_PARAM;
		}
	}

	if(!tmp_size) {
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	if (tmp_size < *size) {
		*size = tmp_size;
	}
	// 2 byte header
	((uint8_t*)data)[0] = (uint8_t)*size;
	((uint8_t*)data)[1] = LIBUSB_DT_STRING;
	memcpy((uint8_t*)data+2, tmp, *size-2);
	return LIBUSB_COMPLETED;
}

static int _hid_get_hid_descriptor(struct hid_device_priv *dev, void *data, size_t *size)
{
	struct libusb_hid_descriptor d;
	uint8_t tmp[MAX_HID_DESCRIPTOR_SIZE];
	size_t report_len = MAX_HID_DESCRIPTOR_SIZE;

	_hid_get_report_descriptor(dev, tmp, &report_len);

	d.bLength = LIBUSB_DT_HID_SIZE;
	d.bDescriptorType = LIBUSB_DT_HID;
	d.bcdHID = 0x0110; /* 1.10 */
	d.bCountryCode = 0;
	d.bNumDescriptors = 1;
	d.bClassDescriptorType = LIBUSB_DT_REPORT;
	d.wClassDescriptorLength = (uint16_t)report_len;

	if (*size > LIBUSB_DT_HID_SIZE)
		*size = LIBUSB_DT_HID_SIZE;
	memcpy(data, &d, *size);
	return LIBUSB_COMPLETED;
}

static int _hid_get_report_descriptor(struct hid_device_priv *dev, void *data, size_t *size)
{
	uint8_t d[MAX_HID_DESCRIPTOR_SIZE];
	size_t i = 0;

	/* usage page (0xFFA0 == vendor defined) */
	d[i++] = 0x06; d[i++] = 0xA0; d[i++] = 0xFF;
	/* usage (vendor defined) */
	d[i++] = 0x09; d[i++] = 0x01;
	/* start collection (application) */
	d[i++] = 0xA1; d[i++] = 0x01;
	/* input report */
	if (dev->input_report_size) {
		/* usage (vendor defined) */
		d[i++] = 0x09; d[i++] = 0x01;
		/* logical minimum (0) */
		d[i++] = 0x15; d[i++] = 0x00;
		/* logical maximum (255) */
		d[i++] = 0x25; d[i++] = 0xFF;
		/* report size (8 bits) */
		d[i++] = 0x75; d[i++] = 0x08;
		/* report count */
		d[i++] = 0x95; d[i++] = (uint8_t)dev->input_report_size - 1;
		/* input (data, variable, absolute) */
		d[i++] = 0x81; d[i++] = 0x00;
	}
	/* output report */
	if (dev->output_report_size) {
		/* usage (vendor defined) */
		d[i++] = 0x09; d[i++] = 0x02;
		/* logical minimum (0) */
		d[i++] = 0x15; d[i++] = 0x00;
		/* logical maximum (255) */
		d[i++] = 0x25; d[i++] = 0xFF;
		/* report size (8 bits) */
		d[i++] = 0x75; d[i++] = 0x08;
		/* report count */
		d[i++] = 0x95; d[i++] = (uint8_t)dev->output_report_size - 1;
		/* output (data, variable, absolute) */
		d[i++] = 0x91; d[i++] = 0x00;
	}
	/* feature report */
	if (dev->feature_report_size) {
		/* usage (vendor defined) */
		d[i++] = 0x09; d[i++] = 0x03;
		/* logical minimum (0) */
		d[i++] = 0x15; d[i++] = 0x00;
		/* logical maximum (255) */
		d[i++] = 0x25; d[i++] = 0xFF;
		/* report size (8 bits) */
		d[i++] = 0x75; d[i++] = 0x08;
		/* report count */
		d[i++] = 0x95; d[i++] = (uint8_t)dev->feature_report_size - 1;
		/* feature (data, variable, absolute) */
		d[i++] = 0xb2; d[i++] = 0x02; d[i++] = 0x01;
	}

	/* end collection */
	d[i++] = 0xC0;

	if (*size > i)
		*size = i;
	memcpy(data, d, *size);
	return LIBUSB_COMPLETED;
}

static int _hid_get_descriptor(struct hid_device_priv *dev, HANDLE hid_handle, int recipient,
							   int type, int _index, void *data, size_t *size)
{
	switch(type) {
	case LIBUSB_DT_DEVICE:
		usbi_dbg("LIBUSB_DT_DEVICE");
		return _hid_get_device_descriptor(dev, data, size);
	case LIBUSB_DT_CONFIG:
		usbi_dbg("LIBUSB_DT_CONFIG");
		if (!_index)
			return _hid_get_config_descriptor(dev, data, size);
		return LIBUSB_ERROR_INVALID_PARAM;
	case LIBUSB_DT_STRING:
		usbi_dbg("LIBUSB_DT_STRING");
		return _hid_get_string_descriptor(dev, _index, data, size);
	case LIBUSB_DT_HID:
		usbi_dbg("LIBUSB_DT_HID");
		if (!_index)
			return _hid_get_hid_descriptor(dev, data, size);
		return LIBUSB_ERROR_INVALID_PARAM;
	case LIBUSB_DT_REPORT:
		usbi_dbg("LIBUSB_DT_REPORT");
		if (!_index)
			return _hid_get_report_descriptor(dev, data, size);
		return LIBUSB_ERROR_INVALID_PARAM;
	case LIBUSB_DT_PHYSICAL:
		usbi_dbg("LIBUSB_DT_PHYSICAL");
		if (HidD_GetPhysicalDescriptor(hid_handle, data, (ULONG)*size))
			return LIBUSB_COMPLETED;
		return LIBUSB_ERROR_OTHER;
	}
	usbi_dbg("unsupported");
	return LIBUSB_ERROR_INVALID_PARAM;
}

static int _hid_get_report(struct hid_device_priv *dev, HANDLE hid_handle, int id, void *data,
						   struct windows_transfer_priv *tp, size_t *size, OVERLAPPED *overlapped,
						   int report_type)
{
	uint8_t *buf;
	DWORD ioctl_code, read_size, expected_size = (DWORD)*size;
	int r = LIBUSB_SUCCESS;

	if (tp->hid_buffer != NULL) {
		usbi_dbg("program assertion failed: hid_buffer is not NULL");
	}

	if ((*size == 0) || (*size > MAX_HID_REPORT_SIZE)) {
		usbi_dbg("invalid size (%d)", *size);
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	switch (report_type) {
		case HID_REPORT_TYPE_INPUT:
			ioctl_code = IOCTL_HID_GET_INPUT_REPORT;
			break;
		case HID_REPORT_TYPE_FEATURE:
			ioctl_code = IOCTL_HID_GET_FEATURE;
			break;
		default:
			usbi_dbg("unknown HID report type %d", report_type);
			return LIBUSB_ERROR_INVALID_PARAM;
	}

	// Add a trailing byte to detect overflows
	buf = (uint8_t*)calloc(expected_size+1, 1);
	if (buf == NULL) {
		return LIBUSB_ERROR_NO_MEM;
	}
	buf[0] = (uint8_t)id;	// Must be set always
	usbi_dbg("report ID: 0x%02X", buf[0]);

	tp->hid_expected_size = expected_size;
	read_size = expected_size;

	// NB: The size returned by DeviceIoControl doesn't include report IDs when not in use (0)
	if (!DeviceIoControl(hid_handle, ioctl_code, buf, expected_size+1,
		buf, expected_size+1, &read_size, overlapped)) {
		if (GetLastError() != ERROR_IO_PENDING) {
			usbi_dbg("Failed to Read HID Report: %s", windows_error_str(0));
			safe_free(buf);
			return LIBUSB_ERROR_IO;
		}
		// Asynchronous wait
		tp->hid_buffer = buf;
		tp->hid_dest = (uint8_t*)data; // copy dest, as not necessarily the start of the transfer buffer
		return LIBUSB_SUCCESS;
	}

	// Transfer completed synchronously => copy and discard extra buffer
	if (read_size == 0) {
		usbi_warn(NULL, "program assertion failed - read completed synchronously, but no data was read");
		*size = 0;
	} else {
		if (buf[0] != id) {
			usbi_warn(NULL, "mismatched report ID (data is %02X, parameter is %02X)", buf[0], id);
		}
		if ((size_t)read_size > expected_size) {
			r = LIBUSB_ERROR_OVERFLOW;
			usbi_dbg("OVERFLOW!");
		} else {
			r = LIBUSB_COMPLETED;
		}

		*size = MIN((size_t)read_size, *size);
		if (id == 0) {
			// Discard report ID
			memcpy(data, buf+1, *size);
		} else {
			memcpy(data, buf, *size);
		}
	}
	safe_free(buf);
	return r;
}

static int _hid_set_report(struct hid_device_priv *dev, HANDLE hid_handle, int id, void *data,
						   struct windows_transfer_priv *tp, size_t *size, OVERLAPPED *overlapped,
						   int report_type)
{
	uint8_t *buf = NULL;
	DWORD ioctl_code, write_size= (DWORD)*size;

	if (tp->hid_buffer != NULL) {
		usbi_dbg("program assertion failed: hid_buffer is not NULL");
	}

	if ((*size == 0) || (*size > MAX_HID_REPORT_SIZE)) {
		usbi_dbg("invalid size (%d)", *size);
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	switch (report_type) {
		case HID_REPORT_TYPE_OUTPUT:
			ioctl_code = IOCTL_HID_SET_OUTPUT_REPORT;
			break;
		case HID_REPORT_TYPE_FEATURE:
			ioctl_code = IOCTL_HID_SET_FEATURE;
			break;
		default:
			usbi_dbg("unknown HID report type %d", report_type);
			return LIBUSB_ERROR_INVALID_PARAM;
	}

	usbi_dbg("report ID: 0x%02X", id);
	// When report IDs are not used (i.e. when id == 0), we must add
	// a null report ID. Otherwise, we just use original data buffer
	if (id == 0) {
		write_size++;
	}
	buf = (uint8_t*) malloc(write_size);
	if (buf == NULL) {
		return LIBUSB_ERROR_NO_MEM;
	}
	if (id == 0) {
		buf[0] = 0;
		memcpy(buf + 1, data, *size);
	} else {
		// This seems like a waste, but if we don't duplicate the
		// data, we'll get issues when freeing hid_buffer
		memcpy(buf, data, *size);
		if (buf[0] != id) {
			usbi_warn(NULL, "mismatched report ID (data is %02X, parameter is %02X)", buf[0], id);
		}
	}

	// NB: The size returned by DeviceIoControl doesn't include report IDs when not in use (0)
	if (!DeviceIoControl(hid_handle, ioctl_code, buf, write_size,
		buf, write_size, &write_size, overlapped)) {
		if (GetLastError() != ERROR_IO_PENDING) {
			usbi_dbg("Failed to Write HID Output Report: %s", windows_error_str(0));
			safe_free(buf);
			return LIBUSB_ERROR_IO;
		}
		tp->hid_buffer = buf;
		tp->hid_dest = NULL;
		return LIBUSB_SUCCESS;
	}

	// Transfer completed synchronously
	*size = write_size;
	if (write_size == 0) {
		usbi_dbg("program assertion failed - write completed synchronously, but no data was written");
	}
	safe_free(buf);
	return LIBUSB_COMPLETED;
}

static int _hid_class_request(struct hid_device_priv *dev, HANDLE hid_handle, int request_type,
							  int request, int value, int _index, void *data, struct windows_transfer_priv *tp,
							  size_t *size, OVERLAPPED *overlapped)
{
	int report_type = (value >> 8) & 0xFF;
	int report_id = value & 0xFF;

	if ( (LIBUSB_REQ_RECIPIENT(request_type) != LIBUSB_RECIPIENT_INTERFACE)
	  && (LIBUSB_REQ_RECIPIENT(request_type) != LIBUSB_RECIPIENT_DEVICE) )
		return LIBUSB_ERROR_INVALID_PARAM;

	if (LIBUSB_REQ_OUT(request_type) && request == HID_REQ_SET_REPORT)
		return _hid_set_report(dev, hid_handle, report_id, data, tp, size, overlapped, report_type);

	if (LIBUSB_REQ_IN(request_type) && request == HID_REQ_GET_REPORT)
		return _hid_get_report(dev, hid_handle, report_id, data, tp, size, overlapped, report_type);

	return LIBUSB_ERROR_INVALID_PARAM;
}


/*
 * HID API functions
 */
static int hid_init(int sub_api, struct libusb_context *ctx)
{
	DLL_LOAD(hid.dll, HidD_GetAttributes, TRUE);
	DLL_LOAD(hid.dll, HidD_GetHidGuid, TRUE);
	DLL_LOAD(hid.dll, HidD_GetPreparsedData, TRUE);
	DLL_LOAD(hid.dll, HidD_FreePreparsedData, TRUE);
	DLL_LOAD(hid.dll, HidD_GetManufacturerString, TRUE);
	DLL_LOAD(hid.dll, HidD_GetProductString, TRUE);
	DLL_LOAD(hid.dll, HidD_GetSerialNumberString, TRUE);
	DLL_LOAD(hid.dll, HidP_GetCaps, TRUE);
	DLL_LOAD(hid.dll, HidD_SetNumInputBuffers, TRUE);
	DLL_LOAD(hid.dll, HidD_SetFeature, TRUE);
	DLL_LOAD(hid.dll, HidD_GetFeature, TRUE);
	DLL_LOAD(hid.dll, HidD_GetPhysicalDescriptor, TRUE);
	DLL_LOAD(hid.dll, HidD_GetInputReport, FALSE);
	DLL_LOAD(hid.dll, HidD_SetOutputReport, FALSE);
	DLL_LOAD(hid.dll, HidD_FlushQueue, TRUE);
	DLL_LOAD(hid.dll, HidP_GetValueCaps, TRUE);

	HidD_GetHidGuid(&HID_guid);

	api_hid_available = true;
	return LIBUSB_SUCCESS;
}

static int hid_exit(int sub_api)
{
	return LIBUSB_SUCCESS;
}

// NB: open and close must ensure that they only handle interface of
// the right API type, as these functions can be called wholesale from
// composite_open(), with interfaces belonging to different APIs
static int hid_open(int sub_api, struct libusb_device_handle *dev_handle)
{
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);

	HIDD_ATTRIBUTES hid_attributes;
	PHIDP_PREPARSED_DATA preparsed_data = NULL;
	HIDP_CAPS capabilities;
	HIDP_VALUE_CAPS *value_caps;

	HANDLE hid_handle = INVALID_HANDLE_VALUE;
	int i, j;
	// report IDs handling
	ULONG size[3];
	const char *type[3] = {"input", "output", "feature"};
	int nb_ids[2];	// zero and nonzero report IDs

	CHECK_HID_AVAILABLE;
	if (priv->hid == NULL) {
		usbi_err(ctx, "program assertion failed - private HID structure is unitialized");
		return LIBUSB_ERROR_NOT_FOUND;
	}

	for (i = 0; i < USB_MAXINTERFACES; i++) {
		if ( (priv->usb_interface[i].path != NULL)
		  && (priv->usb_interface[i].apib->id == USB_API_HID) ) {
			hid_handle = CreateFileA(priv->usb_interface[i].path, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ,
				NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
			/*
			 * http://www.lvr.com/hidfaq.htm: Why do I receive "Access denied" when attempting to access my HID?
			 * "Windows 2000 and later have exclusive read/write access to HIDs that are configured as a system
			 * keyboards or mice. An application can obtain a handle to a system keyboard or mouse by not
			 * requesting READ or WRITE access with CreateFile. Applications can then use HidD_SetFeature and
			 * HidD_GetFeature (if the device supports Feature reports)."
			 */
			if (hid_handle == INVALID_HANDLE_VALUE) {
				usbi_warn(ctx, "could not open HID device in R/W mode (keyboard or mouse?) - trying without");
				hid_handle = CreateFileA(priv->usb_interface[i].path, 0, FILE_SHARE_WRITE | FILE_SHARE_READ,
					NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
				if (hid_handle == INVALID_HANDLE_VALUE) {
					usbi_err(ctx, "could not open device %s (interface %d): %s", priv->path, i, windows_error_str(0));
					switch(GetLastError()) {
					case ERROR_FILE_NOT_FOUND:	// The device was disconnected
						return LIBUSB_ERROR_NO_DEVICE;
					case ERROR_ACCESS_DENIED:
						return LIBUSB_ERROR_ACCESS;
					default:
						return LIBUSB_ERROR_IO;
					}
				}
				priv->usb_interface[i].restricted_functionality = true;
			}
			handle_priv->interface_handle[i].api_handle = hid_handle;
		}
	}

	hid_attributes.Size = sizeof(hid_attributes);
	do {
		if (!HidD_GetAttributes(hid_handle, &hid_attributes)) {
			usbi_err(ctx, "could not gain access to HID top collection (HidD_GetAttributes)");
			break;
		}

		priv->hid->vid = hid_attributes.VendorID;
		priv->hid->pid = hid_attributes.ProductID;

		// Set the maximum available input buffer size
		for (i=32; HidD_SetNumInputBuffers(hid_handle, i); i*=2);
		usbi_dbg("set maximum input buffer size to %d", i/2);

		// Get the maximum input and output report size
		if (!HidD_GetPreparsedData(hid_handle, &preparsed_data) || !preparsed_data) {
			usbi_err(ctx, "could not read HID preparsed data (HidD_GetPreparsedData)");
			break;
		}
		if (HidP_GetCaps(preparsed_data, &capabilities) != HIDP_STATUS_SUCCESS) {
			usbi_err(ctx, "could not parse HID capabilities (HidP_GetCaps)");
			break;
		}

		// Find out if interrupt will need report IDs
		size[0] = capabilities.NumberInputValueCaps;
		size[1] = capabilities.NumberOutputValueCaps;
		size[2] = capabilities.NumberFeatureValueCaps;
		for (j=HidP_Input; j<=HidP_Feature; j++) {
			usbi_dbg("%d HID %s report value(s) found", size[j], type[j]);
			priv->hid->uses_report_ids[j] = false;
			if (size[j] > 0) {
				value_caps = (HIDP_VALUE_CAPS*) calloc(size[j], sizeof(HIDP_VALUE_CAPS));
				if ( (value_caps != NULL)
				  && (HidP_GetValueCaps((HIDP_REPORT_TYPE)j, value_caps, &size[j], preparsed_data) == HIDP_STATUS_SUCCESS)
				  && (size[j] >= 1) ) {
					nb_ids[0] = 0;
					nb_ids[1] = 0;
					for (i=0; i<(int)size[j]; i++) {
						usbi_dbg("  Report ID: 0x%02X", value_caps[i].ReportID);
						if (value_caps[i].ReportID != 0) {
							nb_ids[1]++;
						} else {
							nb_ids[0]++;
						}
					}
					if (nb_ids[1] != 0) {
						if (nb_ids[0] != 0) {
							usbi_warn(ctx, "program assertion failed: zero and nonzero report IDs used for %s",
								type[j]);
						}
						priv->hid->uses_report_ids[j] = true;
					}
				} else {
					usbi_warn(ctx, "  could not process %s report IDs", type[j]);
				}
				safe_free(value_caps);
			}
		}

		// Set the report sizes
		priv->hid->input_report_size = capabilities.InputReportByteLength;
		priv->hid->output_report_size = capabilities.OutputReportByteLength;
		priv->hid->feature_report_size = capabilities.FeatureReportByteLength;

		// Fetch string descriptors
		priv->hid->string_index[0] = priv->dev_descriptor.iManufacturer;
		if (priv->hid->string_index[0] != 0) {
			HidD_GetManufacturerString(hid_handle, priv->hid->string[0],
				sizeof(priv->hid->string[0]));
		} else {
			priv->hid->string[0][0] = 0;
		}
		priv->hid->string_index[1] = priv->dev_descriptor.iProduct;
		if (priv->hid->string_index[1] != 0) {
			HidD_GetProductString(hid_handle, priv->hid->string[1],
				sizeof(priv->hid->string[1]));
		} else {
			priv->hid->string[1][0] = 0;
		}
		priv->hid->string_index[2] = priv->dev_descriptor.iSerialNumber;
		if (priv->hid->string_index[2] != 0) {
			HidD_GetSerialNumberString(hid_handle, priv->hid->string[2],
				sizeof(priv->hid->string[2]));
		} else {
			priv->hid->string[2][0] = 0;
		}
	} while(0);

	if (preparsed_data) {
		HidD_FreePreparsedData(preparsed_data);
	}

	return LIBUSB_SUCCESS;
}

static void hid_close(int sub_api, struct libusb_device_handle *dev_handle)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	HANDLE file_handle;
	int i;

	if (!api_hid_available)
		return;

	for (i = 0; i < USB_MAXINTERFACES; i++) {
		if (priv->usb_interface[i].apib->id == USB_API_HID) {
			file_handle = handle_priv->interface_handle[i].api_handle;
			if ( (file_handle != 0) && (file_handle != INVALID_HANDLE_VALUE)) {
				CloseHandle(file_handle);
			}
		}
	}
}

static int hid_claim_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface)
{
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);

	CHECK_HID_AVAILABLE;

	// NB: Disconnection detection is not possible in this function
	if (priv->usb_interface[iface].path == NULL) {
		return LIBUSB_ERROR_NOT_FOUND;	// invalid iface
	}

	// We use dev_handle as a flag for interface claimed
	if (handle_priv->interface_handle[iface].dev_handle == INTERFACE_CLAIMED) {
		return LIBUSB_ERROR_BUSY;	// already claimed
	}

	handle_priv->interface_handle[iface].dev_handle = INTERFACE_CLAIMED;

	usbi_dbg("claimed interface %d", iface);
	handle_priv->active_interface = iface;

	return LIBUSB_SUCCESS;
}

static int hid_release_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface)
{
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);

	CHECK_HID_AVAILABLE;

	if (priv->usb_interface[iface].path == NULL) {
		return LIBUSB_ERROR_NOT_FOUND;	// invalid iface
	}

	if (handle_priv->interface_handle[iface].dev_handle != INTERFACE_CLAIMED) {
		return LIBUSB_ERROR_NOT_FOUND;	// invalid iface
	}

	handle_priv->interface_handle[iface].dev_handle = INVALID_HANDLE_VALUE;

	return LIBUSB_SUCCESS;
}

static int hid_set_interface_altsetting(int sub_api, struct libusb_device_handle *dev_handle, int iface, int altsetting)
{
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);

	CHECK_HID_AVAILABLE;

	if (altsetting > 255) {
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	if (altsetting != 0) {
		usbi_err(ctx, "set interface altsetting not supported for altsetting >0");
		return LIBUSB_ERROR_NOT_SUPPORTED;
	}

	return LIBUSB_SUCCESS;
}

static int hid_submit_control_transfer(int sub_api, struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(transfer->dev_handle);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	WINUSB_SETUP_PACKET *setup = (WINUSB_SETUP_PACKET *) transfer->buffer;
	HANDLE hid_handle;
	struct winfd wfd;
	int current_interface, config;
	size_t size;
	int r = LIBUSB_ERROR_INVALID_PARAM;

	CHECK_HID_AVAILABLE;

	transfer_priv->pollable_fd = INVALID_WINFD;
	safe_free(transfer_priv->hid_buffer);
	transfer_priv->hid_dest = NULL;
	size = transfer->length - LIBUSB_CONTROL_SETUP_SIZE;

	if (size > MAX_CTRL_BUFFER_LENGTH) {
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	current_interface = get_valid_interface(transfer->dev_handle, USB_API_HID);
	if (current_interface < 0) {
		if (auto_claim(transfer, &current_interface, USB_API_HID) != LIBUSB_SUCCESS) {
			return LIBUSB_ERROR_NOT_FOUND;
		}
	}

	usbi_dbg("will use interface %d", current_interface);
	hid_handle = handle_priv->interface_handle[current_interface].api_handle;
	// Always use the handle returned from usbi_create_fd (wfd.handle)
	wfd = usbi_create_fd(hid_handle, RW_READ, NULL, NULL);
	if (wfd.fd < 0) {
		return LIBUSB_ERROR_NOT_FOUND;
	}

	switch(LIBUSB_REQ_TYPE(setup->request_type)) {
	case LIBUSB_REQUEST_TYPE_STANDARD:
		switch(setup->request) {
		case LIBUSB_REQUEST_GET_DESCRIPTOR:
			r = _hid_get_descriptor(priv->hid, wfd.handle, LIBUSB_REQ_RECIPIENT(setup->request_type),
				(setup->value >> 8) & 0xFF, setup->value & 0xFF, transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE, &size);
			break;
		case LIBUSB_REQUEST_GET_CONFIGURATION:
			r = windows_get_configuration(transfer->dev_handle, &config);
			if (r == LIBUSB_SUCCESS) {
				size = 1;
				((uint8_t*)transfer->buffer)[LIBUSB_CONTROL_SETUP_SIZE] = (uint8_t)config;
				r = LIBUSB_COMPLETED;
			}
			break;
		case LIBUSB_REQUEST_SET_CONFIGURATION:
			if (setup->value == priv->active_config) {
				r = LIBUSB_COMPLETED;
			} else {
				usbi_warn(ctx, "cannot set configuration other than the default one");
				r = LIBUSB_ERROR_INVALID_PARAM;
			}
			break;
		case LIBUSB_REQUEST_GET_INTERFACE:
			size = 1;
			((uint8_t*)transfer->buffer)[LIBUSB_CONTROL_SETUP_SIZE] = 0;
			r = LIBUSB_COMPLETED;
			break;
		case LIBUSB_REQUEST_SET_INTERFACE:
			r = hid_set_interface_altsetting(0, transfer->dev_handle, setup->index, setup->value);
			if (r == LIBUSB_SUCCESS) {
				r = LIBUSB_COMPLETED;
			}
			break;
		default:
			usbi_warn(ctx, "unsupported HID control request");
			r = LIBUSB_ERROR_INVALID_PARAM;
			break;
		}
		break;
	case LIBUSB_REQUEST_TYPE_CLASS:
		r =_hid_class_request(priv->hid, wfd.handle, setup->request_type, setup->request, setup->value,
			setup->index, transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE, transfer_priv,
			&size, wfd.overlapped);
		break;
	default:
		usbi_warn(ctx, "unsupported HID control request");
		r = LIBUSB_ERROR_INVALID_PARAM;
		break;
	}

	if (r == LIBUSB_COMPLETED) {
		// Force request to be completed synchronously. Transferred size has been set by previous call
		wfd.overlapped->Internal = STATUS_COMPLETED_SYNCHRONOUSLY;
		// http://msdn.microsoft.com/en-us/library/ms684342%28VS.85%29.aspx
		// set InternalHigh to the number of bytes transferred
		wfd.overlapped->InternalHigh = (DWORD)size;
		r = LIBUSB_SUCCESS;
	}

	if (r == LIBUSB_SUCCESS) {
		// Use priv_transfer to store data needed for async polling
		transfer_priv->pollable_fd = wfd;
		transfer_priv->interface_number = (uint8_t)current_interface;
	} else {
		usbi_free_fd(&wfd);
	}

	return r;
}

static int hid_submit_bulk_transfer(int sub_api, struct usbi_transfer *itransfer) {
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(transfer->dev_handle);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	struct winfd wfd;
	HANDLE hid_handle;
	bool direction_in, ret;
	int current_interface, length;
	DWORD size;
	int r = LIBUSB_SUCCESS;

	CHECK_HID_AVAILABLE;

	transfer_priv->pollable_fd = INVALID_WINFD;
	transfer_priv->hid_dest = NULL;
	safe_free(transfer_priv->hid_buffer);

	current_interface = interface_by_endpoint(priv, handle_priv, transfer->endpoint);
	if (current_interface < 0) {
		usbi_err(ctx, "unable to match endpoint to an open interface - cancelling transfer");
		return LIBUSB_ERROR_NOT_FOUND;
	}

	usbi_dbg("matched endpoint %02X with interface %d", transfer->endpoint, current_interface);

	hid_handle = handle_priv->interface_handle[current_interface].api_handle;
	direction_in = transfer->endpoint & LIBUSB_ENDPOINT_IN;

	wfd = usbi_create_fd(hid_handle, direction_in?RW_READ:RW_WRITE, NULL, NULL);
	// Always use the handle returned from usbi_create_fd (wfd.handle)
	if (wfd.fd < 0) {
		return LIBUSB_ERROR_NO_MEM;
	}

	// If report IDs are not in use, an extra prefix byte must be added
	if ( ((direction_in) && (!priv->hid->uses_report_ids[0]))
	  || ((!direction_in) && (!priv->hid->uses_report_ids[1])) ) {
		length = transfer->length+1;
	} else {
		length = transfer->length;
	}
	// Add a trailing byte to detect overflows on input
	transfer_priv->hid_buffer = (uint8_t*)calloc(length+1, 1);
	if (transfer_priv->hid_buffer == NULL) {
		return LIBUSB_ERROR_NO_MEM;
	}
	transfer_priv->hid_expected_size = length;

	if (direction_in) {
		transfer_priv->hid_dest = transfer->buffer;
		usbi_dbg("reading %d bytes (report ID: 0x00)", length);
		ret = ReadFile(wfd.handle, transfer_priv->hid_buffer, length+1, &size, wfd.overlapped);
	} else {
		if (!priv->hid->uses_report_ids[1]) {
			memcpy(transfer_priv->hid_buffer+1, transfer->buffer, transfer->length);
		} else {
			// We could actually do without the calloc and memcpy in this case
			memcpy(transfer_priv->hid_buffer, transfer->buffer, transfer->length);
		}
		usbi_dbg("writing %d bytes (report ID: 0x%02X)", length, transfer_priv->hid_buffer[0]);
		ret = WriteFile(wfd.handle, transfer_priv->hid_buffer, length, &size, wfd.overlapped);
	}
	if (!ret) {
		if (GetLastError() != ERROR_IO_PENDING) {
			usbi_err(ctx, "HID transfer failed: %s", windows_error_str(0));
			usbi_free_fd(&wfd);
			safe_free(transfer_priv->hid_buffer);
			return LIBUSB_ERROR_IO;
		}
	} else {
		// Only write operations that completed synchronously need to free up
		// hid_buffer. For reads, copy_transfer_data() handles that process.
		if (!direction_in) {
			safe_free(transfer_priv->hid_buffer);
		}
		if (size == 0) {
			usbi_err(ctx, "program assertion failed - no data was transferred");
			size = 1;
		}
		if (size > (size_t)length) {
			usbi_err(ctx, "OVERFLOW!");
			r = LIBUSB_ERROR_OVERFLOW;
		}
		wfd.overlapped->Internal = STATUS_COMPLETED_SYNCHRONOUSLY;
		wfd.overlapped->InternalHigh = size;
	}

	transfer_priv->pollable_fd = wfd;
	transfer_priv->interface_number = (uint8_t)current_interface;

	return r;
}

static int hid_abort_transfers(int sub_api, struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct windows_transfer_priv *transfer_priv = (struct windows_transfer_priv*)usbi_transfer_get_os_priv(itransfer);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(transfer->dev_handle);
	HANDLE hid_handle;
	int current_interface;

	CHECK_HID_AVAILABLE;

	current_interface = transfer_priv->interface_number;
	hid_handle = handle_priv->interface_handle[current_interface].api_handle;
	CancelIo(hid_handle);

	return LIBUSB_SUCCESS;
}

static int hid_reset_device(int sub_api, struct libusb_device_handle *dev_handle)
{
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	HANDLE hid_handle;
	int current_interface;

	CHECK_HID_AVAILABLE;

	// Flushing the queues on all interfaces is the best we can achieve
	for (current_interface = 0; current_interface < USB_MAXINTERFACES; current_interface++) {
		hid_handle = handle_priv->interface_handle[current_interface].api_handle;
		if ((hid_handle != 0) && (hid_handle != INVALID_HANDLE_VALUE)) {
			HidD_FlushQueue(hid_handle);
		}
	}
	return LIBUSB_SUCCESS;
}

static int hid_clear_halt(int sub_api, struct libusb_device_handle *dev_handle, unsigned char endpoint)
{
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	HANDLE hid_handle;
	int current_interface;

	CHECK_HID_AVAILABLE;

	current_interface = interface_by_endpoint(priv, handle_priv, endpoint);
	if (current_interface < 0) {
		usbi_err(ctx, "unable to match endpoint to an open interface - cannot clear");
		return LIBUSB_ERROR_NOT_FOUND;
	}

	usbi_dbg("matched endpoint %02X with interface %d", endpoint, current_interface);
	hid_handle = handle_priv->interface_handle[current_interface].api_handle;

	// No endpoint selection with Microsoft's implementation, so we try to flush the
	// whole interface. Should be OK for most case scenarios
	if (!HidD_FlushQueue(hid_handle)) {
		usbi_err(ctx, "Flushing of HID queue failed: %s", windows_error_str(0));
		// Device was probably disconnected
		return LIBUSB_ERROR_NO_DEVICE;
	}

	return LIBUSB_SUCCESS;
}

// This extra function is only needed for HID
static int hid_copy_transfer_data(int sub_api, struct usbi_transfer *itransfer, uint32_t io_size) {
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_transfer_priv *transfer_priv = usbi_transfer_get_os_priv(itransfer);
	int r = LIBUSB_TRANSFER_COMPLETED;
	uint32_t corrected_size = io_size;

	if (transfer_priv->hid_buffer != NULL) {
		// If we have a valid hid_buffer, it means the transfer was async
		if (transfer_priv->hid_dest != NULL) {	// Data readout
			if (corrected_size > 0) {
				// First, check for overflow
				if (corrected_size > transfer_priv->hid_expected_size) {
					usbi_err(ctx, "OVERFLOW!");
					corrected_size = (uint32_t)transfer_priv->hid_expected_size;
					r = LIBUSB_TRANSFER_OVERFLOW;
				}

				if (transfer_priv->hid_buffer[0] == 0) {
					// Discard the 1 byte report ID prefix
					corrected_size--;
					memcpy(transfer_priv->hid_dest, transfer_priv->hid_buffer+1, corrected_size);
				} else {
					memcpy(transfer_priv->hid_dest, transfer_priv->hid_buffer, corrected_size);
				}
			}
			transfer_priv->hid_dest = NULL;
		}
		// For write, we just need to free the hid buffer
		safe_free(transfer_priv->hid_buffer);
	}
	itransfer->transferred += corrected_size;
	return r;
}


/*
 * Composite API functions
 */
static int composite_init(int sub_api, struct libusb_context *ctx)
{
	return LIBUSB_SUCCESS;
}

static int composite_exit(int sub_api)
{
	return LIBUSB_SUCCESS;
}

static int composite_open(int sub_api, struct libusb_device_handle *dev_handle)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	int r = LIBUSB_ERROR_NOT_FOUND;
	uint8_t i;
	// SUB_API_MAX+1 as the SUB_API_MAX pos is used to indicate availability of HID
	bool available[SUB_API_MAX+1] = {0};

	for (i=0; i<USB_MAXINTERFACES; i++) {
		switch (priv->usb_interface[i].apib->id) {
		case USB_API_WINUSBX:
			if (priv->usb_interface[i].sub_api != SUB_API_NOTSET)
				available[priv->usb_interface[i].sub_api] = true;
			break;
		case USB_API_HID:
			available[SUB_API_MAX] = true;
			break;
		default:
			break;
		}
	}

	for (i=0; i<SUB_API_MAX; i++) {	// WinUSB-like drivers
		if (available[i]) {
			r = usb_api_backend[USB_API_WINUSBX].open(i, dev_handle);
			if (r != LIBUSB_SUCCESS) {
				return r;
			}
		}
	}
	if (available[SUB_API_MAX]) {	// HID driver
		r = hid_open(SUB_API_NOTSET, dev_handle);
	}
	return r;
}

static void composite_close(int sub_api, struct libusb_device_handle *dev_handle)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	uint8_t i;
	bool available[SUB_API_MAX];

	for (i = 0; i<SUB_API_MAX; i++) {
		available[i] = false;
	}

	for (i=0; i<USB_MAXINTERFACES; i++) {
		if ( (priv->usb_interface[i].apib->id == USB_API_WINUSBX)
		  && (priv->usb_interface[i].sub_api != SUB_API_NOTSET) ) {
			available[priv->usb_interface[i].sub_api] = true;
		}
	}

	for (i=0; i<SUB_API_MAX; i++) {
		if (available[i]) {
			usb_api_backend[USB_API_WINUSBX].close(i, dev_handle);
		}
	}
}

static int composite_claim_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	return priv->usb_interface[iface].apib->
		claim_interface(priv->usb_interface[iface].sub_api, dev_handle, iface);
}

static int composite_set_interface_altsetting(int sub_api, struct libusb_device_handle *dev_handle, int iface, int altsetting)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	return priv->usb_interface[iface].apib->
		set_interface_altsetting(priv->usb_interface[iface].sub_api, dev_handle, iface, altsetting);
}

static int composite_release_interface(int sub_api, struct libusb_device_handle *dev_handle, int iface)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	return priv->usb_interface[iface].apib->
		release_interface(priv->usb_interface[iface].sub_api, dev_handle, iface);
}

static int composite_submit_control_transfer(int sub_api, struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	int i, pass;

	// Interface shouldn't matter for control, but it does in practice, with Windows'
	// restrictions with regards to accessing HID keyboards and mice. Try a 2 pass approach
	for (pass = 0; pass < 2; pass++) {
		for (i=0; i<USB_MAXINTERFACES; i++) {
			if (priv->usb_interface[i].path != NULL) {
				if ((pass == 0) && (priv->usb_interface[i].restricted_functionality)) {
					usbi_dbg("trying to skip restricted interface #%d (HID keyboard or mouse?)", i);
					continue;
				}
				usbi_dbg("using interface %d", i);
				return priv->usb_interface[i].apib->submit_control_transfer(priv->usb_interface[i].sub_api, itransfer);
			}
		}
	}

	usbi_err(ctx, "no libusb supported interfaces to complete request");
	return LIBUSB_ERROR_NOT_FOUND;
}

static int composite_submit_bulk_transfer(int sub_api, struct usbi_transfer *itransfer) {
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(transfer->dev_handle);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	int current_interface;

	current_interface = interface_by_endpoint(priv, handle_priv, transfer->endpoint);
	if (current_interface < 0) {
		usbi_err(ctx, "unable to match endpoint to an open interface - cancelling transfer");
		return LIBUSB_ERROR_NOT_FOUND;
	}

	return priv->usb_interface[current_interface].apib->
		submit_bulk_transfer(priv->usb_interface[current_interface].sub_api, itransfer);}

static int composite_submit_iso_transfer(int sub_api, struct usbi_transfer *itransfer) {
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct libusb_context *ctx = DEVICE_CTX(transfer->dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(transfer->dev_handle);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);
	int current_interface;

	current_interface = interface_by_endpoint(priv, handle_priv, transfer->endpoint);
	if (current_interface < 0) {
		usbi_err(ctx, "unable to match endpoint to an open interface - cancelling transfer");
		return LIBUSB_ERROR_NOT_FOUND;
	}

	return priv->usb_interface[current_interface].apib->
		submit_iso_transfer(priv->usb_interface[current_interface].sub_api, itransfer);}

static int composite_clear_halt(int sub_api, struct libusb_device_handle *dev_handle, unsigned char endpoint)
{
	struct libusb_context *ctx = DEVICE_CTX(dev_handle->dev);
	struct windows_device_handle_priv *handle_priv = _device_handle_priv(dev_handle);
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	int current_interface;

	current_interface = interface_by_endpoint(priv, handle_priv, endpoint);
	if (current_interface < 0) {
		usbi_err(ctx, "unable to match endpoint to an open interface - cannot clear");
		return LIBUSB_ERROR_NOT_FOUND;
	}

	return priv->usb_interface[current_interface].apib->
		clear_halt(priv->usb_interface[current_interface].sub_api, dev_handle, endpoint);}

static int composite_abort_control(int sub_api, struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct windows_transfer_priv *transfer_priv = usbi_transfer_get_os_priv(itransfer);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);

	return priv->usb_interface[transfer_priv->interface_number].apib->
		abort_control(priv->usb_interface[transfer_priv->interface_number].sub_api, itransfer);}

static int composite_abort_transfers(int sub_api, struct usbi_transfer *itransfer)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct windows_transfer_priv *transfer_priv = usbi_transfer_get_os_priv(itransfer);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);

	return priv->usb_interface[transfer_priv->interface_number].apib->
		abort_transfers(priv->usb_interface[transfer_priv->interface_number].sub_api, itransfer);}

static int composite_reset_device(int sub_api, struct libusb_device_handle *dev_handle)
{
	struct windows_device_priv *priv = _device_priv(dev_handle->dev);
	int r;
	uint8_t i; 
	bool available[SUB_API_MAX];
	for (i = 0; i<SUB_API_MAX; i++) {
		available[i] = false;
	}
	for (i=0; i<USB_MAXINTERFACES; i++) {
		if ( (priv->usb_interface[i].apib->id == USB_API_WINUSBX)
		  && (priv->usb_interface[i].sub_api != SUB_API_NOTSET) ) {
			available[priv->usb_interface[i].sub_api] = true;
		}
	}
	for (i=0; i<SUB_API_MAX; i++) {
		if (available[i]) {
			r = usb_api_backend[USB_API_WINUSBX].reset_device(i, dev_handle);
			if (r != LIBUSB_SUCCESS) {
				return r;
			}
		}
	}
	return LIBUSB_SUCCESS;
}

static int composite_copy_transfer_data(int sub_api, struct usbi_transfer *itransfer, uint32_t io_size)
{
	struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	struct windows_transfer_priv *transfer_priv = usbi_transfer_get_os_priv(itransfer);
	struct windows_device_priv *priv = _device_priv(transfer->dev_handle->dev);

	return priv->usb_interface[transfer_priv->interface_number].apib->
		copy_transfer_data(priv->usb_interface[transfer_priv->interface_number].sub_api, itransfer, io_size);
}
