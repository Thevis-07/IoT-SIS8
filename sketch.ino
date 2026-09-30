/*
 * Sensor de ocupação e desperdício de luz — ESP32 no Wokwi
 * PIR (GPIO 27) + LDR módulo AO (GPIO 34) + LED (GPIO 2)
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ================= CONFIGURAÇÃO =================
// URL do seu Worker, SEM barra no final
const char* WORKER_URL = "https://<seu-worker>.workers.dev";

const int PINO_PIR = 27;
const int PINO_LDR = 34;
const int PINO_LED = 2;

// R1 — Calibração: ligue MODO_CALIBRACAO, mova o lux do LDR para o máximo
// e para o mínimo, anote os valores do Serial e preencha abaixo.
const bool MODO_CALIBRACAO   = false;
const int  LEITURA_LUZ_ALTA  = 32;   
const int  LEITURA_LUZ_BAIXA = 4063;    
const int  LIMIAR_LUZ = (LEITURA_LUZ_ALTA + LEITURA_LUZ_BAIXA) / 2; 

// Tempos (ms)
const unsigned long TEMPO_OCUPADA_MS     = 30000;  // R3: ocupada até 30 s após o último movimento
const unsigned long TEMPO_DESPERDICIO_MS = 60000;  // R3: vazia + luz acesa por mais de 60 s
const unsigned long INTERVALO_LUZ_MS     = 20000;  // R4: POST /luz a cada 20 s
const unsigned long INTERVALO_PISCA_MS   = 300;    // velocidade do pisca
const unsigned long ESPERA_REENVIO_MS    = 3000;   // espera antes de reenviar um movimento que falhou
const unsigned long INTERVALO_CALIB_MS   = 500;

// ================= INTERRUPÇÃO (R2) =================
volatile uint32_t      contadorMovimentos = 0;
volatile unsigned long ultimoMovimentoMs  = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR aoDetectarMovimento() {
  portENTER_CRITICAL_ISR(&mux);
  contadorMovimentos++;
  ultimoMovimentoMs = millis();
  portEXIT_CRITICAL_ISR(&mux);
}

void lerMovimentos(uint32_t &total, unsigned long &ultimo) {
  portENTER_CRITICAL(&mux);
  total  = contadorMovimentos;
  ultimo = ultimoMovimentoMs;
  portEXIT_CRITICAL(&mux);
}

uint32_t movimentosMostrados = 0;   
uint32_t movimentosEnviados  = 0;   
unsigned long ultimaFalhaMovMs = 0;
bool falhouUltimoEnvioMov = false;

unsigned long ultimoEnvioLuzMs = 0;
unsigned long ultimaCalibMs    = 0;

bool vaziaComLuz = false;               
unsigned long inicioVaziaComLuzMs = 0;  

enum Estado { VAZIA, OCUPADA, DESPERDICIO };
Estado estadoAtual = VAZIA;
bool primeiroEstado = true;

bool luzEstaAcesa(int valor) {
  if (LEITURA_LUZ_ALTA > LEITURA_LUZ_BAIXA) return valor > LIMIAR_LUZ;
  return valor < LIMIAR_LUZ;
}

const char* nomeEstado(int e) {   
  switch (e) {
    case OCUPADA:     return "OCUPADA";
    case DESPERDICIO: return "DESPERDICIO (vazia com luz acesa > 60 s)";
    default:          return "VAZIA";
  }
}

void conectarWiFi() {
  Serial.print("[WiFi] Conectando");
  WiFi.begin("Wokwi-GUEST", "", 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\n[WiFi] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
}

// POST HTTPS para o Worker. Retorna o código HTTP (ou negativo em erro).
int enviarPost(const char* rota, const String& corpo) {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return -1;
  }
  WiFiClientSecure client;
  client.setInsecure();                 
  HTTPClient http;
  http.begin(client, String(WORKER_URL) + rota);
  http.addHeader("Content-Type", "application/json");
  int codigo = http.POST(corpo);
  http.end();
  return codigo;
}

void setup() {
  Serial.begin(115200);
  pinMode(PINO_PIR, INPUT);
  pinMode(PINO_LED, OUTPUT);
  digitalWrite(PINO_LED, LOW);

  Serial.println("\n=== Sensor de sala — IoT ===");
  Serial.printf("[CONFIG] Luz alta=%d | Luz baixa=%d | LIMIAR_LUZ=%d\n",
                LEITURA_LUZ_ALTA, LEITURA_LUZ_BAIXA, LIMIAR_LUZ);

  conectarWiFi();

  attachInterrupt(digitalPinToInterrupt(PINO_PIR), aoDetectarMovimento, RISING);
  Serial.println("[PIR] Interrupção ativa no GPIO 27 (RISING)");
}

void loop() {
  uint32_t totalMov;
  unsigned long ultimoMov;
  lerMovimentos(totalMov, ultimoMov);
  unsigned long agora = millis();       

  while (movimentosMostrados < totalMov) {
    movimentosMostrados++;
    Serial.printf("[MOVIMENTO] Detectado! Total acumulado: %u\n", movimentosMostrados);
  }

  int valorLuz = analogRead(PINO_LDR);
  bool acesa = luzEstaAcesa(valorLuz);

  if (MODO_CALIBRACAO && agora - ultimaCalibMs >= INTERVALO_CALIB_MS) {
    ultimaCalibMs = agora;
    Serial.printf("[CALIBRACAO] analogRead(34) = %d\n", valorLuz);
  }

  bool ocupada = (totalMov > 0) && (agora - ultimoMov < TEMPO_OCUPADA_MS);

  if (!ocupada && acesa) {
    if (!vaziaComLuz) {               
      vaziaComLuz = true;
      inicioVaziaComLuzMs = agora;
    }
  } else {
    vaziaComLuz = false;
  }
  bool desperdicio = vaziaComLuz && (agora - inicioVaziaComLuzMs > TEMPO_DESPERDICIO_MS);

  Estado novoEstado = desperdicio ? DESPERDICIO : (ocupada ? OCUPADA : VAZIA);
  if (primeiroEstado || novoEstado != estadoAtual) {
    estadoAtual = novoEstado;
    primeiroEstado = false;
    Serial.printf("[ESTADO] Sala %s | luz %s (%d)\n",
                  nomeEstado(estadoAtual), acesa ? "ACESA" : "APAGADA", valorLuz);
  }

  if (estadoAtual == OCUPADA) {
    digitalWrite(PINO_LED, HIGH);
  } else if (estadoAtual == DESPERDICIO) {
    digitalWrite(PINO_LED, (agora / INTERVALO_PISCA_MS) % 2);   
  } else {
    digitalWrite(PINO_LED, LOW);
  }

  bool podeTentar = !falhouUltimoEnvioMov || (agora - ultimaFalhaMovMs >= ESPERA_REENVIO_MS);
  if (movimentosEnviados < totalMov && podeTentar) {
    int codigo = enviarPost("/movimento", "{}");
    if (codigo >= 200 && codigo < 300) {
      movimentosEnviados++;
      falhouUltimoEnvioMov = false;
    } else {
      falhouUltimoEnvioMov = true;
      ultimaFalhaMovMs = millis();
    }
    lerMovimentos(totalMov, ultimoMov);
    Serial.printf("[HTTP] POST /movimento -> %d | enviados: %u | pendentes: %u\n",
                  codigo, movimentosEnviados, totalMov - movimentosEnviados);
  }

  if (millis() - ultimoEnvioLuzMs >= INTERVALO_LUZ_MS) {
    ultimoEnvioLuzMs = millis();
    String corpo = "{\"valor\":" + String(valorLuz) + ",\"acesa\":" + String(acesa ? 1 : 0) + "}";
    int codigo = enviarPost("/luz", corpo);
    Serial.printf("[LUZ] leitura=%d (%s, limiar=%d) | sala %s | POST /luz -> %d\n",
                  valorLuz, acesa ? "ACESA" : "APAGADA", LIMIAR_LUZ,
                  nomeEstado(estadoAtual), codigo);
  }

  delay(10);
}
