#pragma once

#include <linux/videodev2.h>

namespace wave::v4l2ext {

constexpr unsigned priv(unsigned n) { return V4L2_CID_PRIVATE_BASE + n; }
constexpr unsigned camClass(unsigned n) { return V4L2_CID_CAMERA_CLASS_BASE + n; }

constexpr unsigned CID_ROTATION = priv(0);
constexpr unsigned CID_PADDR_Y = priv(1);
constexpr unsigned CID_PADDR_CBCR = priv(4);
constexpr unsigned CID_STREAM_PAUSE = priv(53);

constexpr unsigned CID_JPEG_MAIN_SIZE = priv(32);
constexpr unsigned CID_JPEG_MAIN_OFFSET = priv(33);
constexpr unsigned CID_JPEG_POSTVIEW_OFFSET = priv(36);
constexpr unsigned CID_JPEG_QUALITY = priv(37);
constexpr unsigned CID_VT_MODE = priv(48);
constexpr unsigned CID_VGA_BLUR = priv(49);
constexpr unsigned CID_CAPTURE = priv(50);
constexpr unsigned CID_SCENE_MODE = priv(70);
constexpr unsigned CID_FLASH_MODE = priv(71);
constexpr unsigned CID_BRIGHTNESS = priv(72);
constexpr unsigned CID_WHITE_BALANCE = priv(73);
constexpr unsigned CID_EFFECT = priv(74);
constexpr unsigned CID_ISO = priv(75);
constexpr unsigned CID_METERING = priv(76);
constexpr unsigned CID_CONTRAST = priv(77);
constexpr unsigned CID_SATURATION = priv(78);
constexpr unsigned CID_SHARPNESS = priv(79);
constexpr unsigned CID_WDR = priv(80);
constexpr unsigned CID_ANTI_SHAKE = priv(81);
constexpr unsigned CID_TOUCH_AF_START_STOP = priv(82);
constexpr unsigned CID_GPS_LATITUDE = camClass(30);
constexpr unsigned CID_GPS_LONGITUDE = camClass(31);
constexpr unsigned CID_GPS_TIMESTAMP = camClass(32);
constexpr unsigned CID_GPS_ALTITUDE = camClass(33);
constexpr unsigned CID_EXIF_TIME_INFO = camClass(34);
constexpr unsigned CID_GPS_PROCESSINGMETHOD = camClass(35);
constexpr unsigned CID_ZOOM = priv(90);
constexpr unsigned CID_FACE_DETECTION = priv(91);
constexpr unsigned CID_SET_AUTO_FOCUS = priv(93);
constexpr unsigned CID_BEAUTY_SHOT = priv(94);
constexpr unsigned CID_OBJECT_POSITION_X = priv(97);
constexpr unsigned CID_OBJECT_POSITION_Y = priv(98);
constexpr unsigned CID_FOCUS_MODE = priv(99);
constexpr unsigned CID_CAF_START_STOP = priv(102);
constexpr unsigned CID_AUTO_FOCUS_RESULT_FIRST = priv(103);
constexpr unsigned CID_FRAME_RATE = priv(104);
constexpr unsigned CID_ANTI_BANDING = priv(105);
constexpr unsigned CID_SET_GAMMA = priv(106);
constexpr unsigned CID_SET_SLOW_AE = priv(107);
constexpr unsigned CID_BATCH_REFLECTION = priv(108);
constexpr unsigned CID_EXIF_ORIENTATION = priv(109);
constexpr unsigned CID_CHECK_DATALINE = priv(112);
constexpr unsigned CID_CHECK_DATALINE_STOP = priv(113);
constexpr unsigned CID_GET_ISO = priv(114);
constexpr unsigned CID_GET_SHT_TIME = priv(115);
constexpr unsigned CID_GET_FLASH_ONOFF = priv(118);
constexpr unsigned CID_RETURN_FOCUS = priv(119);
constexpr unsigned CID_FINISH_AUTO_FOCUS = priv(121);

constexpr unsigned PIX_FMT_NV12T = v4l2_fourcc('T', 'V', '1', '2');

enum Blur { BLUR_LEVEL_0 = 0, BLUR_LEVEL_MAX = 4 };

enum SceneMode {
    SCENE_MODE_BASE, SCENE_MODE_NONE, SCENE_MODE_PORTRAIT, SCENE_MODE_NIGHTSHOT,
    SCENE_MODE_BACK_LIGHT, SCENE_MODE_LANDSCAPE, SCENE_MODE_SPORTS,
    SCENE_MODE_PARTY_INDOOR, SCENE_MODE_BEACH_SNOW, SCENE_MODE_SUNSET,
    SCENE_MODE_DUST_DAWN, SCENE_MODE_FALL_COLOR, SCENE_MODE_FIREWORKS,
    SCENE_MODE_TEXT, SCENE_MODE_CANDLE_LIGHT, SCENE_MODE_MAX,
};

enum FlashMode {
    FLASH_MODE_BASE, FLASH_MODE_OFF, FLASH_MODE_AUTO, FLASH_MODE_ON,
    FLASH_MODE_TORCH, FLASH_MODE_MAX,
};

enum Ev { EV_MINUS_4 = 0, EV_DEFAULT = 4, EV_PLUS_4 = 8 };

enum WhiteBalance {
    WHITE_BALANCE_BASE, WHITE_BALANCE_AUTO, WHITE_BALANCE_SUNNY,
    WHITE_BALANCE_CLOUDY, WHITE_BALANCE_TUNGSTEN, WHITE_BALANCE_FLUORESCENT,
    WHITE_BALANCE_MAX,
};

enum Effect {
    IMAGE_EFFECT_BASE, IMAGE_EFFECT_NONE, IMAGE_EFFECT_BNW, IMAGE_EFFECT_SEPIA,
    IMAGE_EFFECT_AQUA, IMAGE_EFFECT_ANTIQUE, IMAGE_EFFECT_NEGATIVE,
    IMAGE_EFFECT_SHARPEN, IMAGE_EFFECT_MAX,
};

enum Iso {
    ISO_AUTO, ISO_50, ISO_100, ISO_200, ISO_400, ISO_800, ISO_1600,
    ISO_SPORTS, ISO_NIGHT, ISO_MOVIE, ISO_MAX,
};

enum Metering { METERING_BASE, METERING_MATRIX, METERING_CENTER, METERING_SPOT, METERING_MAX };

enum Level5 { LEVEL_MINUS_2 = 0, LEVEL_DEFAULT = 2, LEVEL_MAX = 5 };

enum Wdr { WDR_OFF, WDR_ON, WDR_MAX };
enum AntiShake { ANTI_SHAKE_OFF, ANTI_SHAKE_MAX = 3 };
enum ZoomLevel { ZOOM_LEVEL_0 = 0, ZOOM_LEVEL_MAX = 13 };

enum FaceDetection {
    FACE_DETECTION_OFF, FACE_DETECTION_ON, FACE_DETECTION_NOLINE,
    FACE_DETECTION_ON_BEAUTY, FACE_DETECTION_MAX,
};

enum AutoFocus { AUTO_FOCUS_OFF, AUTO_FOCUS_ON };

enum FocusMode {
    FOCUS_MODE_AUTO, FOCUS_MODE_MACRO, FOCUS_MODE_FACEDETECT,
    FOCUS_MODE_AUTO_DEFAULT, FOCUS_MODE_MACRO_DEFAULT,
    FOCUS_MODE_FACEDETECT_DEFAULT, FOCUS_MODE_INFINITY, FOCUS_MODE_MAX,
};

enum FrameRate { FRAME_RATE_AUTO = 0, FRAME_RATE_MAX = 121 };

enum AntiBanding { ANTI_BANDING_AUTO, ANTI_BANDING_50HZ, ANTI_BANDING_60HZ, ANTI_BANDING_OFF };
enum Gamma { GAMMA_OFF, GAMMA_ON, GAMMA_MAX };
enum SlowAe { SLOW_AE_OFF, SLOW_AE_ON };

struct sec_cam_parm {
    v4l2_captureparm capture;
    int contrast;
    int effects;
    int brightness;
    int flash_mode;
    int focus_mode;
    int iso;
    int metering;
    int saturation;
    int scene_mode;
    int sharpness;
    int white_balance;
};
static_assert(sizeof(sec_cam_parm) <= sizeof(v4l2_streamparm::parm));

struct gps_info {
    unsigned sign;
    unsigned degree;
    unsigned minute;
    unsigned second;
};

}
