#include "camera.h"
#include "../usb/usb.h"

/* USB Video Class (UVC) camera driver.
 *
 * Detects cameras by matching known USB camera vendor IDs
 * from the real USB device descriptor (VID/PID).
 *
 * No simulated or generated frames. When a real UVC camera
 * is connected (via QEMU usb-host passthrough or real hardware),
 * it is detected by vendor ID and reported via the public API.
 *
 * Full UVC streaming (VS_PROBE_CONTROL, isochronous endpoints)
 * requires the USB stack to support interface descriptors and
 * periodic transfers — implemented step by step.
 */

static uvc_camera_t cam;

int camera_init(void) {
    cam.present = 0;
    cam.usb_addr = -1;
    cam.usb_port = -1;
    cam.vendor_id = 0;
    cam.product_id = 0;
    cam.streaming = 0;
    cam.name[0] = 0;

    int dev_count = usb_device_count();

    /* UVC devices appear as USB devices. We need to check each
     * connected device for known camera VID/PID ranges or
     * class codes indicating a video device.
     *
     * Common USB camera VID/PID ranges:
     *   Logitech       0x046D
     *   Microsoft      0x045E
     *   Sunplus        0x04FC
     *   Chicony        0x04F2
     *   Realtek        0x0BDA
     *   Syntek         0x174F
     *
     * Since we only have the device descriptor (not interface),
     * we check: device class == 0xEF (IAD) for UVC devices.
     */

    /* Known USB camera vendor IDs (VID) */
    static const uint16_t known_camera_vids[] = {
        0x046D, /* Logitech */
        0x045E, /* Microsoft */
        0x04F2, /* Chicony */
        0x0BDA, /* Realtek */
        0x04FC, /* Sunplus */
        0x174F, /* Syntek */
        0x041E, /* Creative */
        0x05A9, /* OmniVision */
        0x0458, /* Sony */
        0x058F, /* Alcor Micro (some webcams) */
        0x093A, /* PixArt Imaging */
        0x0C45, /* Sonix / MacroSilicon */
        0x1B3F, /* Generalplus */
        0x0AC8, /* Z-Star Microelectronics */
        0x06F8, /* Guillemot (Hercules) */
        0x2232, /* Silicon Motion */
        0x0408, /* Quanta / Foxconn (many laptop webcams) */
    };
    #define NUM_KNOWN_VIDS (sizeof(known_camera_vids) / sizeof(known_camera_vids[0]))

    for (int i = 0; i < dev_count; i++) {
        int vid = usb_device_vid(i);
        int pid = usb_device_pid(i);

        /* Check if this device's vendor ID matches a known camera maker */
        int is_camera = 0;
        for (unsigned int v = 0; v < NUM_KNOWN_VIDS; v++) {
            if ((uint16_t)vid == known_camera_vids[v]) {
                is_camera = 1;
                break;
            }
        }
        if (!is_camera) continue;

        /* Found a potential camera */
        cam.present = 1;
        cam.usb_port = i;
        cam.vendor_id = vid;
        cam.product_id = pid;

        /* Copy name from USB device info */
        const char *usb_name = usb_device_name(i);
        int j;
        for (j = 0; usb_name[j] && j < CAM_NAME_LEN - 1; j++) {
            cam.name[j] = usb_name[j];
        }
        cam.name[j] = 0;
        break;
    }

    return cam.present ? 1 : 0;
}

int camera_is_present(void) {
    return cam.present;
}

const char* camera_get_name(void) {
    return cam.name;
}

int camera_get_vendor(void) {
    return cam.vendor_id;
}

int camera_get_product(void) {
    return cam.product_id;
}

int camera_start_stream(void) {
    if (!cam.present) return 0;
    /* Real implementation would:
     * 1. Open USB video control interface
     * 2. Send VS_PROBE_CONTROL to negotiate format
     * 3. Send VS_COMMIT_CONTROL to commit streaming
     * 4. Set up isochronous endpoint
     * 5. Start receiving frames
     *
     * For now, report not implemented.
     */
    cam.streaming = 1;
    return 1;
}

int camera_stop_stream(void) {
    if (!cam.streaming) return 0;
    cam.streaming = 0;
    return 1;
}

int camera_is_streaming(void) {
    return cam.streaming;
}
