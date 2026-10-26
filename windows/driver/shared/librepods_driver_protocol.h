/*
 * Derived from librepods-windows by ivLis-Studio
 * (https://github.com/ivLis-Studio/librepods-windows), GPLv3 — a Windows port of
 * LibrePods (https://github.com/kavishdevar/librepods).
 */
#ifndef LIBREPODS_DRIVER_PROTOCOL_H
#define LIBREPODS_DRIVER_PROTOCOL_H

typedef unsigned char librepods_u8;
typedef unsigned int librepods_u32;
typedef unsigned __int64 librepods_u64;

#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022u
#endif

#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS 0u
#endif

#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0u
#endif

#ifndef CTL_CODE
#define CTL_CODE(device_type, function, method, access) \
    ((((device_type) << 16) | ((access) << 14) | ((function) << 2) | (method)))
#endif

#define LIBREPODS_DRIVER_PROTOCOL_VERSION 3u
#define LIBREPODS_DRIVER_INTERFACE_GUID_STRING L"{3E98D13A-B2A5-4C3D-AE1E-8DB4F3D8C1F0}"
#define LIBREPODS_DRIVER_DEVICE_PATH_ENVIRONMENT L"LIBREPODS_DRIVER_DEVICE_PATH"
#define LIBREPODS_DRIVER_MAX_BLUETOOTH_ADDRESS_LENGTH 18u
#define LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH 512u

/* L2CAP PSMs the app can request via open_session_request.psm (v2+).
 * AACP is the AirPods vendor control channel; ATT carries raw ATT PDUs for
 * hearing-aid/transparency characteristics. psm=0 defaults to AACP. */
#define LIBREPODS_PSM_AACP 0x1001u
#define LIBREPODS_PSM_ATT  0x001Fu

#define LIBREPODS_IOCTL_GET_BRIDGE_STATUS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LIBREPODS_IOCTL_OPEN_AACP_CHANNEL \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LIBREPODS_IOCTL_CLOSE_AACP_CHANNEL \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LIBREPODS_IOCTL_SEND_AACP_PACKET \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LIBREPODS_IOCTL_READ_AACP_PACKET \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* SDP record publish/remove — forwarded to IOCTL_BTH_SDP_* on the local
 * radio, which user-mode callers cannot reach directly (ERROR_NOT_SUPPORTED).
 * Used to publish a host Device ID record claiming Apple vendor identity so
 * the AirPods arm the ATT (PSM 31) listener. */
#define LIBREPODS_IOCTL_SUBMIT_SDP_RECORD \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LIBREPODS_IOCTL_REMOVE_SDP_RECORD \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

enum librepods_driver_capability_flags {
    LIBREPODS_DRIVER_CAPABILITY_NONE = 0,
    LIBREPODS_DRIVER_CAPABILITY_USER_MODE_BRIDGE = 1 << 0,
    LIBREPODS_DRIVER_CAPABILITY_DRIVER_BACKED_L2CAP = 1 << 1,
    LIBREPODS_DRIVER_CAPABILITY_AACP_CONTROL = 1 << 2,
    LIBREPODS_DRIVER_CAPABILITY_BATTERY_NOTIFICATIONS = 1 << 3,
    /* v3: OPEN/READ requests are pended in the driver and honor CancelIoEx —
     * a wedged open/read releases the caller immediately instead of waiting
     * out the BRB timeout, and process teardown can never strand a thread. */
    LIBREPODS_DRIVER_CAPABILITY_CANCELABLE_IO = 1 << 4
};

enum librepods_driver_error_code {
    LIBREPODS_DRIVER_ERROR_NONE = 0,
    LIBREPODS_DRIVER_ERROR_NOT_IMPLEMENTED = 1,
    LIBREPODS_DRIVER_ERROR_DRIVER_UNAVAILABLE = 2,
    LIBREPODS_DRIVER_ERROR_ACCESS_DENIED = 3,
    LIBREPODS_DRIVER_ERROR_TRANSPORT_BLOCKED = 4,
    LIBREPODS_DRIVER_ERROR_INTERNAL = 5,
    LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT = 6
};

typedef struct librepods_driver_status {
    librepods_u32 struct_size;
    librepods_u32 protocol_version;
    librepods_u32 capability_flags;
    librepods_u32 max_packet_size;
    librepods_u32 last_error;
    librepods_u32 reserved0;
    librepods_u64 reserved1;
} librepods_driver_status;

typedef struct librepods_driver_open_session_request {
    librepods_u32 struct_size;
    char bluetooth_address[LIBREPODS_DRIVER_MAX_BLUETOOTH_ADDRESS_LENGTH];
    /* v2: requested L2CAP PSM (0 => LIBREPODS_PSM_AACP). Drivers built for
     * protocol v1 only see/validate the fields before this member. */
    librepods_u32 psm;
} librepods_driver_open_session_request;

typedef struct librepods_driver_open_session_response {
    librepods_u32 struct_size;
    librepods_u32 capability_flags;
    librepods_u32 mtu;
    librepods_u32 reserved0;
    librepods_u64 session_id;
} librepods_driver_open_session_response;

typedef struct librepods_driver_close_session_request {
    librepods_u32 struct_size;
    librepods_u32 reserved0;
    librepods_u64 session_id;
} librepods_driver_close_session_request;

typedef struct librepods_driver_send_packet_request {
    librepods_u32 struct_size;
    librepods_u32 packet_length;
    librepods_u32 reserved0;
    librepods_u32 reserved1;
    librepods_u64 session_id;
    librepods_u8 packet[LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH];
} librepods_driver_send_packet_request;

typedef struct librepods_driver_read_packet_request {
    librepods_u32 struct_size;
    librepods_u32 packet_buffer_length;
    librepods_u32 reserved0;
    librepods_u32 reserved1;
    librepods_u64 session_id;
} librepods_driver_read_packet_request;

typedef struct librepods_driver_read_packet_response {
    librepods_u32 struct_size;
    librepods_u32 packet_length;
    librepods_u32 reserved0;
    librepods_u32 reserved1;
    librepods_u8 packet[LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH];
} librepods_driver_read_packet_response;

#define LIBREPODS_DRIVER_MAX_SDP_RECORD_LENGTH 1024u

typedef struct librepods_driver_sdp_submit_request {
    librepods_u32 struct_size;
    librepods_u32 record_length;
    librepods_u32 reserved0;
    librepods_u32 reserved1;
    librepods_u8 record[LIBREPODS_DRIVER_MAX_SDP_RECORD_LENGTH];
} librepods_driver_sdp_submit_request;

typedef struct librepods_driver_sdp_submit_response {
    librepods_u32 struct_size;
    librepods_u32 reserved0;
    librepods_u64 sdp_handle;
} librepods_driver_sdp_submit_response;

typedef struct librepods_driver_sdp_remove_request {
    librepods_u32 struct_size;
    librepods_u32 reserved0;
    librepods_u64 sdp_handle;
} librepods_driver_sdp_remove_request;

#endif
