#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>

/* UVC (USB Video Class) camera driver
 *
 * Detects USB Video Class devices via the real USB subsystem.
 * When a UVC device is connected, performs control transfers
 * to query capabilities and (in future) stream video.
 *
 * No simulated or generated frames — only real hardware.
 */

#define CAM_MAX_DEVICES 4
#define CAM_NAME_LEN    48

typedef struct {
    int  present;           /* Camera hardware found */
    int  usb_addr;          /* USB device address on UHCI bus */
    int  usb_port;          /* USB port number */
    char name[CAM_NAME_LEN];/* Device name / description */
    int  vendor_id;
    int  product_id;
    int  streaming;         /* 1 if streaming is active */
} uvc_camera_t;

/* Public API */
int  camera_init(void);            /* Scan USB for UVC devices */
int  camera_is_present(void);      /* 1 if camera hardware detected */
const char* camera_get_name(void); /* Human-readable name */
int  camera_get_vendor(void);
int  camera_get_product(void);
int  camera_start_stream(void);    /* Begin video streaming (future) */
int  camera_stop_stream(void);     /* Stop video streaming */
int  camera_is_streaming(void);    /* 1 if actively streaming */

#endif
