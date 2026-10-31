#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"
#include "FS.h"
#include "SD_MMC.h"
#include <WebServer.h>
#include <ESPmDNS.h>

const int PIN_ALARMA = 13;                       // viene del GPIO 19 del controlador
const unsigned long ESPERA_ENTRE_FOTOS_MS = 3000;

WebServer servidorFotos(8080);
int contadorFotos = 0;
bool sdLista = false;
bool alarmaAnterior = false;
unsigned long ultimaFotoMs = 0;

String nombreFoto(int n) {
  return "/foto_" + String(n) + ".jpg";
}

void iniciarSD() {
  // Modo 1-bit: deja libres el flash (GPIO 4) y el GPIO 13
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("No se pudo iniciar la SD (insertada? FAT32?)");
    return;
  }
  sdLista = true;
  while (SD_MMC.exists(nombreFoto(contadorFotos + 1))) contadorFotos++;
  Serial.printf("SD lista. Fotos existentes: %d\n", contadorFotos);
}

void tomarFoto() {
  if (!sdLista) {
    Serial.println("Sin SD: no se guarda la foto");
    return;
  }
  // Descartar un cuadro viejo y capturar uno nuevo
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Fallo al capturar la foto");
    return;
  }

  String ruta = nombreFoto(contadorFotos + 1);
  File archivo = SD_MMC.open(ruta, FILE_WRITE);
  if (!archivo) {
    Serial.println("No se pudo crear el archivo en la SD");
    esp_camera_fb_return(fb);
    return;
  }
  archivo.write(fb->buf, fb->len);
  archivo.close();
  esp_camera_fb_return(fb);

  contadorFotos++;
  Serial.printf("Foto guardada: %s\n", ruta.c_str());
}

void paginaInicio() {
  String html = "<html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<meta http-equiv='refresh' content='5'><title>Alarma</title></head>"
                "<body style='font-family:sans-serif'><h2>Fotos de alarma</h2>";
  if (contadorFotos == 0) {
    html += "<p>Todavia no hay fotos.</p>";
  } else {
    html += "<p>Ultima foto (#" + String(contadorFotos) + "):</p>";
    html += "<img src='" + nombreFoto(contadorFotos) + "' style='max-width:100%'>";
    html += "<p>Anteriores: ";
    for (int n = contadorFotos - 1; n >= 1 && n >= contadorFotos - 10; n--) {
      html += "<a href='" + nombreFoto(n) + "'>#" + String(n) + "</a> ";
    }
    html += "</p>";
  }
  html += "</body></html>";
  servidorFotos.send(200, "text/html", html);
}

void servirArchivo() {
  String ruta = servidorFotos.uri();
  if (sdLista && ruta.endsWith(".jpg") && SD_MMC.exists(ruta)) {
    File f = SD_MMC.open(ruta, FILE_READ);
    servidorFotos.streamFile(f, "image/jpeg");
    f.close();
  } else {
    servidorFotos.send(404, "text/plain", "No encontrado");
  }
}
// ===========================
// Enter your WiFi credentials
// ===========================
const char *ssid = "EmmaP";
const char *password = "samsung1234";

void startCameraServer();
void setupLedFlash();

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

 

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_SVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  // Red propia de la cámara (IP fija 192.168.4.1) + conexión al WiFi si existe
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Camara-Alarma", "alarma1234");   // clave de 8 caracteres o más
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.print("Conectando al WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("Red propia: Camara-Alarma  ->  http://192.168.4.1:8080");
  Serial.println("");
  Serial.println("WiFi connected");

   //codigo prueba
    servidorFotos.on("/foto", []() {
    tomarFoto();
    servidorFotos.sendHeader("Location", "/");
    servidorFotos.send(302, "text/plain", "");
  });

  startCameraServer();
    iniciarSD();
  pinMode(PIN_ALARMA, INPUT_PULLDOWN);
  servidorFotos.on("/", paginaInicio);
  servidorFotos.onNotFound(servirArchivo);
  servidorFotos.begin();
    if (MDNS.begin("camara")) {
    MDNS.addService("http", "tcp", 8080);
    Serial.println("Nombre: http://camara.local:8080");
  } else {
    Serial.println("No se pudo iniciar mDNS");
  }
  Serial.print("Fotos de alarma en: http://");
  Serial.print(WiFi.localIP());
  Serial.println(":8080");

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
}

void loop() {
  servidorFotos.handleClient();

  bool alarma = digitalRead(PIN_ALARMA);
  if (alarma && !alarmaAnterior && millis() - ultimaFotoMs > ESPERA_ENTRE_FOTOS_MS) {
    Serial.println("Alarma recibida: sacando foto");
    tomarFoto();
    ultimaFotoMs = millis();
  }
  alarmaAnterior = alarma;
  delay(10);
}

