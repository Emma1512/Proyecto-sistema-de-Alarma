// Sistema de seguridad: micrófono + ultrasónico + LED + buzzer + ESP32-CAM + teclado
// Controlador: ESP32

#include <Keypad.h>

// ---------- PINES ----------
const int PIN_MIC    = 34;   // A0 del módulo de sonido (3.3V)
const int TRIG       = 16;
const int ECHO       = 17;   // con divisor de tensión (el HC-SR04 saca 5V)
const int PIN_LED    = 18;   // LED de alarma (con resistencia)
const int PIN_CAM    = 33;   // señal a la ESP32-CAM (IO13)
const int PIN_BUZZER = 27;   // buzzer activo (con resistencia de 220 ohm en serie)

// ---------- TECLADO 4x4 ----------
const byte FILAS = 4;
const byte COLUMNAS = 4;
char teclas[FILAS][COLUMNAS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
// Usá los pines que te funcionaron en la prueba del teclado
byte pinesFilas[FILAS]       = {32, 26, 25, 23};
byte pinesColumnas[COLUMNAS] = {22, 21, 14, 13};
Keypad keypad = Keypad(makeKeymap(teclas), pinesFilas, pinesColumnas, FILAS, COLUMNAS);

// ---------- CONFIGURACIÓN ----------
const String CLAVE = "2859";                   // sin 1, 4, 7, *, C ni #
const String TECLAS_IGNORADAS = "147*";        // columna 1: poco confiable
const char TECLA_BORRAR = 'C';
const unsigned long TIEMPO_SALIDA_MS = 10000;  // tiempo para alejarte al armar (0 = sin espera)

// Sonido
const int   VENTANA_MS     = 50;
const int   CALIBRACION_MS = 3000;
const float FACTOR_UMBRAL  = 2.0;

// Ultrasónico
const unsigned long TIEMPO_ESCUCHA = 30000UL;
const float CAMBIO_DISTANCIA_CM    = 10.0;
const int   LECTURAS_CONFIRMAR_US  = 2;
const int   MUESTRAS_BASE          = 11;

// Cámara
const unsigned long PULSO_CAM_MS     = 300;
const unsigned long ESPERA_CAMARA_MS = 30000;  // el pin queda libre mientras la cámara arranca y monta la SD

// Buzzer
// true  = suena hasta que se desactive la alarma con la clave
// false = suena solo TIEMPO_BUZZER_MS y se apaga
const bool BUZZER_HASTA_DESACTIVAR = true;
const unsigned long TIEMPO_BUZZER_MS = 3000;

// ---------- ESTADO Y VARIABLES ----------
enum Estado { DESARMADO, ARMADO, ALARMA };
Estado estado = DESARMADO;

int   umbralSonido  = 0;
float distanciaBase = -1.0;
int   contadorUS = 0;

String entrada = "";
bool   entradaLista = false;

unsigned long inicioAlarma = 0;
bool buzzerSonando = false;
bool camLiberada = false;   // true cuando el pin PIN_CAM ya quedó como salida en LOW

// ---------- CÁMARA ----------
// Durante los primeros 30 s el pin queda libre (INPUT) para no interferir con la SD de la cámara
void actualizarPinCamara() {
  if (!camLiberada && millis() >= ESPERA_CAMARA_MS) {
    pinMode(PIN_CAM, OUTPUT);
    digitalWrite(PIN_CAM, LOW);
    camLiberada = true;
  }
}

// ---------- TECLADO ----------
void leerTeclado() {
  char k = keypad.getKey();
  if (!k) return;

  // Teclas poco confiables: se ignoran para que no ensucien la clave
  if (TECLAS_IGNORADAS.indexOf(k) >= 0) return;

  if (k == '#') {
    entradaLista = true;
  } else if (k == TECLA_BORRAR) {
    entrada = "";
    Serial.println("\nEntrada borrada");
  } else if (entrada.length() < 8) {
    entrada += k;
    Serial.print('*');   // no se muestra la clave
  }
}

void pitidos(int cantidad, int ms) {
  for (int i = 0; i < cantidad; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(ms);
    digitalWrite(PIN_BUZZER, LOW);
    delay(ms);
  }
}

// ---------- SONIDO ----------
int medirAmplitud() {
  int minimo = 4095, maximo = 0;
  unsigned long inicio = millis();
  while (millis() - inicio < VENTANA_MS) {
    int lectura = analogRead(PIN_MIC);
    if (lectura < minimo) minimo = lectura;
    if (lectura > maximo) maximo = lectura;
    leerTeclado();   // así no se pierden teclas mientras mide
  }
  return maximo - minimo;
}

// ---------- ULTRASÓNICO ----------
float medirDistancia() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  unsigned long duracion = pulseIn(ECHO, HIGH, TIEMPO_ESCUCHA);
  if (duracion == 0) return -1.0;

  float distanciaCM = duracion / 58.0;
  if (distanciaCM <= 0 || distanciaCM >= 400) return -1.0;
  return distanciaCM;
}

// ---------- CALIBRACIÓN ----------
void calibrar() {
  Serial.println("Calibrando... no te muevas ni hagas ruido por 3 segundos");

  int maxFondo = 0;
  unsigned long inicio = millis();
  while (millis() - inicio < CALIBRACION_MS) {
    int a = medirAmplitud();
    if (a > maxFondo) maxFondo = a;
  }
  umbralSonido = maxFondo * FACTOR_UMBRAL;

  float muestras[MUESTRAS_BASE];
  int validas = 0;
  for (int i = 0; i < MUESTRAS_BASE; i++) {
    float d = medirDistancia();
    if (d > 0) muestras[validas++] = d;
    delay(60);
  }
  if (validas == 0) {
    distanciaBase = -1.0;
  } else {
    for (int i = 0; i < validas - 1; i++)
      for (int j = i + 1; j < validas; j++)
        if (muestras[j] < muestras[i]) {
          float t = muestras[i]; muestras[i] = muestras[j]; muestras[j] = t;
        }
    distanciaBase = muestras[validas / 2];
  }

  contadorUS = 0;

  Serial.print("Ruido de fondo (max): ");
  Serial.println(maxFondo);
  Serial.print("Umbral de sonido: ");
  Serial.println(umbralSonido);
  if (distanciaBase > 0) {
    Serial.print("Distancia de referencia: ");
    Serial.print(distanciaBase);
    Serial.println(" cm");
  } else {
    Serial.println("AVISO: sin referencia de distancia. Ultrasonico desactivado hasta rearmar.");
  }
}

// ---------- ARMAR / DESARMAR / ALARMA ----------
void armar() {
  Serial.println("Clave correcta. Armando...");
  pitidos(2, 100);

  if (TIEMPO_SALIDA_MS > 0) {
    Serial.print("Tenes ");
    Serial.print(TIEMPO_SALIDA_MS / 1000);
    Serial.println(" segundos para alejarte");
    delay(TIEMPO_SALIDA_MS);
  }

  calibrar();
  entrada = "";
  entradaLista = false;
  estado = ARMADO;
  Serial.println("Sistema ARMADO");
}

void desarmar() {
  digitalWrite(PIN_LED, LOW);
  digitalWrite(PIN_BUZZER, LOW);
  buzzerSonando = false;
  estado = DESARMADO;
  Serial.println("Clave correcta. Sistema DESARMADO");
  pitidos(1, 300);
}

void dispararAlarma(const char* motivo) {
  Serial.println();
  Serial.println("!!! ALARMA !!!");
  Serial.print("Motivo: ");
  Serial.println(motivo);

  estado = ALARMA;
  digitalWrite(PIN_LED, HIGH);      // queda prendido hasta desactivar
  digitalWrite(PIN_BUZZER, HIGH);   // suena hasta desactivar (o TIEMPO_BUZZER_MS)
  inicioAlarma = millis();
  buzzerSonando = true;

  // Pulso hacia la ESP32-CAM para que saque la foto
  pinMode(PIN_CAM, OUTPUT);
  digitalWrite(PIN_CAM, HIGH);
  delay(PULSO_CAM_MS);
  digitalWrite(PIN_CAM, LOW);
  camLiberada = true;

  entrada = "";
  entradaLista = false;
  while (Serial.available()) Serial.read();
  Serial.println("Ingresa la clave y # para desactivar");
}

void procesarClave() {
  bool correcta = (entrada == CLAVE);
  entrada = "";
  entradaLista = false;
  Serial.println();

  if (!correcta) {
    Serial.println("Clave incorrecta");
    if (estado == DESARMADO) pitidos(1, 400);   // con el sistema armado no suena para no activar el micrófono
    return;
  }

  if (estado == DESARMADO) armar();
  else desarmar();   // ARMADO o ALARMA
}

// ---------- VIGILANCIA ----------
void vigilar() {
  // 1) Sonido
  int amplitud = medirAmplitud();
  if (entradaLista) return;   // si se ingresó una clave, tiene prioridad
  if (amplitud > umbralSonido) {
    Serial.print("Amplitud: ");
    Serial.println(amplitud);
    dispararAlarma("ruido superior al doble del ambiente");
    return;
  }

  // 2) Ultrasónico (cambio de distancia)
  float d = medirDistancia();
  if (distanciaBase > 0 && d > 0) {
    if (fabs(d - distanciaBase) >= CAMBIO_DISTANCIA_CM) contadorUS++;
    else contadorUS = 0;

    if (contadorUS >= LECTURAS_CONFIRMAR_US) {
      Serial.print("Distancia: ");
      Serial.print(d);
      Serial.print(" cm (referencia ");
      Serial.print(distanciaBase);
      Serial.println(" cm)");
      dispararAlarma("cambio de distancia de 10 cm o mas");
      return;
    }
  }
}

void manejarAlarma() {
  if (!BUZZER_HASTA_DESACTIVAR && buzzerSonando && millis() - inicioAlarma >= TIEMPO_BUZZER_MS) {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerSonando = false;
  }
  // Respaldo: enviar 'r' por el Monitor Serie también desactiva
  if (Serial.available() && Serial.read() == 'r') desarmar();
  delay(10);
}

// ---------- SETUP / LOOP ----------
void setup() {
  Serial.begin(115200);
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_CAM, INPUT);   // libre mientras la cámara arranca (no interfiere con su SD)
  digitalWrite(PIN_LED, LOW);
  digitalWrite(PIN_BUZZER, LOW);
  delay(500);
  Serial.println("Sistema DESARMADO. Ingresa la clave y # para armar");
}

void loop() {
  actualizarPinCamara();
  leerTeclado();
  if (entradaLista) procesarClave();

  switch (estado) {
    case DESARMADO: delay(10); break;
    case ARMADO:    vigilar(); break;
    case ALARMA:    manejarAlarma(); break;
  }
}