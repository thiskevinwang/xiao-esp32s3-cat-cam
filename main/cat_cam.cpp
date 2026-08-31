#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <vector>

#include "cat_detect.hpp"
#include "dl_image_define.hpp"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "usb_device_uvc.h"

namespace {

constexpr char TAG[] = "cat_cam";

constexpr int CAMERA_PIN_PWDN = -1;
constexpr int CAMERA_PIN_RESET = -1;
constexpr int CAMERA_PIN_XCLK = 10;
constexpr int CAMERA_PIN_SIOD = 40;
constexpr int CAMERA_PIN_SIOC = 39;
constexpr int CAMERA_PIN_D7 = 48;
constexpr int CAMERA_PIN_D6 = 11;
constexpr int CAMERA_PIN_D5 = 12;
constexpr int CAMERA_PIN_D4 = 14;
constexpr int CAMERA_PIN_D3 = 16;
constexpr int CAMERA_PIN_D2 = 18;
constexpr int CAMERA_PIN_D1 = 17;
constexpr int CAMERA_PIN_D0 = 15;
constexpr int CAMERA_PIN_VSYNC = 38;
constexpr int CAMERA_PIN_HREF = 47;
constexpr int CAMERA_PIN_PCLK = 13;

constexpr int CAMERA_WIDTH = 320;
constexpr int CAMERA_HEIGHT = 240;
constexpr int CAMERA_XCLK_HZ = 20000000;
constexpr int BOX_LINE_WIDTH = 4;
constexpr uint8_t JPEG_QUALITY = 80;
constexpr size_t UVC_TRANSFER_BUFFER_SIZE = 96 * 1024;

struct CatCamContext {
    SemaphoreHandle_t camera_lock = nullptr;
    CatDetect *detector = nullptr;
    bool camera_started = false;
    uint8_t *jpeg_buffer = nullptr;
    uvc_fb_t uvc_frame = {};
    uint32_t frame_count = 0;
    size_t last_detection_count = 0;
};

CatCamContext s_cat_cam;

void set_rgb565be_pixel(uint8_t *buffer, int width, int x, int y)
{
    const size_t offset = static_cast<size_t>((y * width + x) * 2);
    buffer[offset] = 0x07;
    buffer[offset + 1] = 0xE0;
}

void draw_cat_box(uint8_t *buffer, int width, int height, const std::vector<int> &box)
{
    if (box.size() < 4) {
        return;
    }

    const int left = std::clamp(box[0], 0, width - 1);
    const int top = std::clamp(box[1], 0, height - 1);
    const int right = std::clamp(box[2], 0, width - 1);
    const int bottom = std::clamp(box[3], 0, height - 1);
    if (right <= left || bottom <= top) {
        return;
    }

    for (int line = 0; line < BOX_LINE_WIDTH; ++line) {
        const int y1 = std::min(top + line, bottom);
        const int y2 = std::max(bottom - line, top);
        for (int x = left; x <= right; ++x) {
            set_rgb565be_pixel(buffer, width, x, y1);
            set_rgb565be_pixel(buffer, width, x, y2);
        }

        const int x1 = std::min(left + line, right);
        const int x2 = std::max(right - line, left);
        for (int y = top; y <= bottom; ++y) {
            set_rgb565be_pixel(buffer, width, x1, y);
            set_rgb565be_pixel(buffer, width, x2, y);
        }
    }
}

void camera_deinit_locked(CatCamContext *context)
{
    if (!context->camera_started) {
        return;
    }

    esp_camera_deinit();
    context->camera_started = false;
    ESP_LOGI(TAG, "camera stopped");
}

esp_err_t camera_init_locked(CatCamContext *context)
{
    camera_deinit_locked(context);

    camera_config_t config = {};
    config.pin_pwdn = CAMERA_PIN_PWDN;
    config.pin_reset = CAMERA_PIN_RESET;
    config.pin_xclk = CAMERA_PIN_XCLK;
    config.pin_sccb_sda = CAMERA_PIN_SIOD;
    config.pin_sccb_scl = CAMERA_PIN_SIOC;
    config.pin_d7 = CAMERA_PIN_D7;
    config.pin_d6 = CAMERA_PIN_D6;
    config.pin_d5 = CAMERA_PIN_D5;
    config.pin_d4 = CAMERA_PIN_D4;
    config.pin_d3 = CAMERA_PIN_D3;
    config.pin_d2 = CAMERA_PIN_D2;
    config.pin_d1 = CAMERA_PIN_D1;
    config.pin_d0 = CAMERA_PIN_D0;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;
    config.pin_pclk = CAMERA_PIN_PCLK;
    config.xclk_freq_hz = CAMERA_XCLK_HZ;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    ESP_RETURN_ON_ERROR(esp_camera_init(&config), TAG, "camera initialization failed");

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != nullptr) {
        sensor->set_vflip(sensor, 1);
        if (sensor->id.PID == OV3660_PID) {
            sensor->set_brightness(sensor, 1);
            sensor->set_saturation(sensor, -2);
        }
    }

    context->camera_started = true;
    ESP_LOGI(TAG, "camera started at %dx%d RGB565", CAMERA_WIDTH, CAMERA_HEIGHT);
    return ESP_OK;
}

esp_err_t camera_start_cb(uvc_format_t format, int width, int height, int rate, void *cb_context)
{
    auto *context = static_cast<CatCamContext *>(cb_context);
    if (format != UVC_FORMAT_JPEG || width != CAMERA_WIDTH || height != CAMERA_HEIGHT) {
        ESP_LOGE(TAG, "unsupported UVC mode: format=%d size=%dx%d", format, width, height);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "host requested %dx%d MJPEG at %d FPS", width, height, rate);
    xSemaphoreTake(context->camera_lock, portMAX_DELAY);
    const esp_err_t result = camera_init_locked(context);
    xSemaphoreGive(context->camera_lock);
    return result;
}

void camera_stop_cb(void *cb_context)
{
    auto *context = static_cast<CatCamContext *>(cb_context);
    xSemaphoreTake(context->camera_lock, portMAX_DELAY);
    camera_deinit_locked(context);
    xSemaphoreGive(context->camera_lock);
}

uvc_fb_t *camera_frame_get_cb(void *cb_context)
{
    auto *context = static_cast<CatCamContext *>(cb_context);
    xSemaphoreTake(context->camera_lock, portMAX_DELAY);

    if (!context->camera_started) {
        xSemaphoreGive(context->camera_lock);
        return nullptr;
    }

    camera_fb_t *camera_frame = esp_camera_fb_get();
    if (camera_frame == nullptr) {
        ESP_LOGE(TAG, "camera capture failed");
        xSemaphoreGive(context->camera_lock);
        return nullptr;
    }

    if (camera_frame->format != PIXFORMAT_RGB565 ||
        camera_frame->width != CAMERA_WIDTH || camera_frame->height != CAMERA_HEIGHT) {
        ESP_LOGE(TAG, "unexpected camera frame format or size");
        esp_camera_fb_return(camera_frame);
        xSemaphoreGive(context->camera_lock);
        return nullptr;
    }

    dl::image::img_t image = {};
    image.data = camera_frame->buf;
    image.width = static_cast<uint16_t>(camera_frame->width);
    image.height = static_cast<uint16_t>(camera_frame->height);
    image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;

    auto &detections = context->detector->run(image);
    for (auto &detection : detections) {
        detection.limit_box(image.width, image.height);
        draw_cat_box(camera_frame->buf, image.width, image.height, detection.box);
    }

    uint8_t *jpeg_buffer = nullptr;
    size_t jpeg_length = 0;
    const bool encoded = frame2jpg(camera_frame, JPEG_QUALITY, &jpeg_buffer, &jpeg_length);
    const timeval timestamp = camera_frame->timestamp;
    esp_camera_fb_return(camera_frame);
    xSemaphoreGive(context->camera_lock);

    if (!encoded || jpeg_buffer == nullptr || jpeg_length == 0) {
        ESP_LOGE(TAG, "JPEG encoding failed");
        free(jpeg_buffer);
        return nullptr;
    }
    if (jpeg_length > UVC_TRANSFER_BUFFER_SIZE) {
        ESP_LOGW(TAG, "encoded frame is too large: %zu bytes", jpeg_length);
        free(jpeg_buffer);
        return nullptr;
    }

    context->jpeg_buffer = jpeg_buffer;
    context->uvc_frame.buf = jpeg_buffer;
    context->uvc_frame.len = jpeg_length;
    context->uvc_frame.width = image.width;
    context->uvc_frame.height = image.height;
    context->uvc_frame.format = UVC_FORMAT_JPEG;
    context->uvc_frame.timestamp = timestamp;
    ++context->frame_count;

    if (context->last_detection_count != detections.size() || context->frame_count % 50 == 0) {
        ESP_LOGI(TAG, "frame=%" PRIu32 " cats=%zu jpeg=%zu bytes",
                 context->frame_count, detections.size(), jpeg_length);
        context->last_detection_count = detections.size();
    }

    return &context->uvc_frame;
}

void camera_frame_return_cb(uvc_fb_t *frame, void *cb_context)
{
    auto *context = static_cast<CatCamContext *>(cb_context);
    if (frame != &context->uvc_frame) {
        ESP_LOGE(TAG, "unexpected UVC frame returned");
        return;
    }

    free(context->jpeg_buffer);
    context->jpeg_buffer = nullptr;
    context->uvc_frame.buf = nullptr;
    context->uvc_frame.len = 0;
}

uint8_t *allocate_uvc_buffer()
{
    auto *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(UVC_TRANSFER_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        buffer = static_cast<uint8_t *>(heap_caps_malloc(UVC_TRANSFER_BUFFER_SIZE, MALLOC_CAP_8BIT));
    }
    return buffer;
}

} // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "loading ESPDet-Pico 224x224 cat model");
    s_cat_cam.camera_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_cat_cam.camera_lock == nullptr ? ESP_ERR_NO_MEM : ESP_OK);

    s_cat_cam.detector = new CatDetect(CatDetect::ESPDET_PICO_224_224_CAT, false);

    uint8_t *uvc_buffer = allocate_uvc_buffer();
    ESP_ERROR_CHECK(uvc_buffer == nullptr ? ESP_ERR_NO_MEM : ESP_OK);

    uvc_device_config_t uvc_config = {};
    uvc_config.uvc_buffer = uvc_buffer;
    uvc_config.uvc_buffer_size = UVC_TRANSFER_BUFFER_SIZE;
    uvc_config.start_cb = camera_start_cb;
    uvc_config.fb_get_cb = camera_frame_get_cb;
    uvc_config.fb_return_cb = camera_frame_return_cb;
    uvc_config.stop_cb = camera_stop_cb;
    uvc_config.cb_ctx = &s_cat_cam;

    ESP_ERROR_CHECK(uvc_device_config(0, &uvc_config));
    ESP_ERROR_CHECK(uvc_device_init());
    ESP_LOGI(TAG, "XIAO ESP32S3 Cat Cam is ready");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
