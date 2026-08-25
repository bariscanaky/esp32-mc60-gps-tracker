#include <Arduino.h>
#include "credentials.h" // IO_USERNAME, IO_KEY, FEED_NAME, SIM_APN, TARGET_PHONE ve AUTHORIZED_NUMBERS içermelidir.

// --- Pin Definitions ---
const int MC60_PWRKEY_PIN = 45;
const int MC60_RX_PIN = 18;
const int MC60_TX_PIN = 17;

// --- Hardware Serial ---
HardwareSerial MC60Serial(1);

// --- Timers & Intervals ---
const unsigned long UPLOAD_INTERVAL_MS = 10000; // 10 seconds
const unsigned long GPS_FIX_TIMEOUT_MS = 60000; // 60 seconds

// --- Adafruit IO ---
const char* ADAFRUIT_SERVER = "io.adafruit.com";
const int ADAFRUIT_PORT = 1883;

// --- Authorized Senders ---
const char* const kAuthorized[] = AUTHORIZED_NUMBERS;
const size_t kAuthorizedCount = sizeof(kAuthorized) / sizeof(kAuthorized[0]);

// --- State Machine ---
enum ModuleState {
    STATE_POWER_ON,
    STATE_WAIT_FOR_BOOT,
    STATE_CHECK_SIM,
    STATE_INIT_SMS_ROUTING,
    STATE_CHECK_NETWORK,
    STATE_ATTACH_GPRS,
    STATE_SET_APN,
    STATE_MQTT_OPEN,
    STATE_MQTT_CONNECT,
    STATE_ENABLE_GPS,
    STATE_WAIT_GPS_FIX,
    STATE_RUNNING,
    STATE_FATAL_ERROR
};
ModuleState currentState = STATE_POWER_ON;

// --- Timers, Flags & Buffers ---
unsigned long stateTimer = 0;
unsigned long lastUploadTime = 0;
int retryCounter = 0;
const int MAX_RETRIES = 5;
bool gpsEnabled = false;

String mc60Buffer = ""; // Gelen anlık SMS'leri yakalamak için global hafıza

// --- Function Prototypes ---
String sendCommand(String cmd, unsigned long timeout, bool print=true);
void checkForSMS();
void sendSMS(String number, String text);
void sendGPSviaSMS(String replyTo);
bool getGPSCoordinates(float &lat, float &lon);
void publishToAdafruitIO(float lat, float lon);
void powerOffModule();
bool isAuthorized(String number);

void setup() {
    Serial.begin(115200);
    Serial.println("\n--- MC60 Direct SMS & GPS Tracker ---");

    if (String(IO_USERNAME) == "YOUR_ADAFRUIT_USERNAME") {
        Serial.println("Set your credentials in credentials.h");
        while(true) delay(1000);
    }

    MC60Serial.begin(115200, SERIAL_8N1, MC60_RX_PIN, MC60_TX_PIN);
    pinMode(MC60_PWRKEY_PIN, OUTPUT);
    digitalWrite(MC60_PWRKEY_PIN, LOW);
    stateTimer = millis();

    Serial.print("Authorized senders: ");
    Serial.println(kAuthorizedCount);
}

void loop() {
    unsigned long now = millis();

    // Gelen seri veriyi sürekli oku ve buffer'a ekle (SMS kaçırmamak için)
    while(MC60Serial.available()) {
        mc60Buffer += (char)MC60Serial.read();
    }
    checkForSMS(); 

    switch (currentState) {
        case STATE_POWER_ON:
            Serial.println("State: POWER_ON");
            digitalWrite(MC60_PWRKEY_PIN, HIGH);
            delay(1000);
            digitalWrite(MC60_PWRKEY_PIN, LOW);
            currentState = STATE_WAIT_FOR_BOOT;
            stateTimer = now;
            break;

        case STATE_WAIT_FOR_BOOT:
            if (now - stateTimer > 3000) {
                String resp = sendCommand("AT", 1000);
                if (resp.indexOf("OK") != -1) {
                    Serial.println("Module responsive.");
                    currentState = STATE_CHECK_SIM;
                    retryCounter = 0;
                } else {
                    Serial.println("No response, retry power-on...");
                    currentState = STATE_POWER_ON;
                }
                stateTimer = now;
            }
            break;

        case STATE_CHECK_SIM:
            if (sendCommand("AT+CPIN?", 3000).indexOf("+CPIN: READY") != -1) {
                Serial.println("SIM ready.");
                currentState = STATE_INIT_SMS_ROUTING;
                retryCounter = 0;
            } else {
                Serial.println("Waiting for SIM...");
                retryCounter++;
                if (retryCounter >= MAX_RETRIES) currentState = STATE_FATAL_ERROR;
            }
            stateTimer = now;
            break;

        case STATE_INIT_SMS_ROUTING:
            // SMS modunu Text (1) olarak ayarla
            sendCommand("AT+CMGF=1", 2000);
            // SMS'leri SIM'e kaydetme, direkt seri porta fırlat
            sendCommand("AT+CNMI=2,2,0,0,0", 2000);
            Serial.println("Direct SMS Routing Enabled. Messages will not be saved on SIM.");
            currentState = STATE_CHECK_NETWORK;
            stateTimer = now;
            break;

        case STATE_CHECK_NETWORK:
            {
                String resp = sendCommand("AT+CREG?", 2000);
                if (resp.indexOf(",1") != -1 || resp.indexOf(",5") != -1) {
                    Serial.println("Network registered.");
                    currentState = STATE_ATTACH_GPRS;
                } else {
                    Serial.println("Waiting for network...");
                    retryCounter++;
                    if (retryCounter >= MAX_RETRIES*2) currentState = STATE_FATAL_ERROR;
                }
                stateTimer = now;
            }
            break;

        case STATE_ATTACH_GPRS:
            sendCommand("AT+CGATT=1", 3000);
            if (sendCommand("AT+CGATT?", 2000).indexOf("+CGATT: 1") != -1) {
                Serial.println("GPRS attached.");
                currentState = STATE_SET_APN;
            } else {
                Serial.println("Waiting for GPRS attachment...");
                retryCounter++;
                if (retryCounter >= MAX_RETRIES) currentState = STATE_FATAL_ERROR;
            }
            stateTimer = now;
            break;

        case STATE_SET_APN:
            if (sendCommand("AT+QIREGAPP=\"" + String(SIM_APN) + "\",\"\",\"\"", 5000).indexOf("OK") != -1) {
                Serial.println("APN set.");
                currentState = STATE_MQTT_OPEN;
            } else {
                Serial.println("Failed to set APN.");
                retryCounter++;
                if (retryCounter >= MAX_RETRIES) currentState = STATE_FATAL_ERROR;
            }
            stateTimer = now;
            break;

        case STATE_MQTT_OPEN:
            if (sendCommand("AT+QMTOPEN=0,\"" + String(ADAFRUIT_SERVER) + "\"," + String(ADAFRUIT_PORT), 10000).indexOf("+QMTOPEN: 0,0") != -1) {
                Serial.println("MQTT socket opened.");
                currentState = STATE_MQTT_CONNECT;
            } else {
                Serial.println("MQTT open failed, retrying...");
                retryCounter++;
                if (retryCounter >= MAX_RETRIES) currentState = STATE_FATAL_ERROR;
                sendCommand("AT+QMTCLOSE=0", 5000);
            }
            stateTimer = now;
            break;

        case STATE_MQTT_CONNECT:
            if (sendCommand("AT+QMTCONN=0,\"gps-tracker\",\"" + String(IO_USERNAME) + "\",\"" + String(IO_KEY) + "\"", 10000).indexOf("+QMTCONN: 0,0,0") != -1) {
                Serial.println("MQTT connected.");
                currentState = STATE_ENABLE_GPS;
            } else {
                Serial.println("MQTT connect failed.");
                retryCounter++;
                if (retryCounter >= MAX_RETRIES) currentState = STATE_FATAL_ERROR;
                sendCommand("AT+QMTCLOSE=0", 5000);
            }
            stateTimer = now;
            break;

        case STATE_ENABLE_GPS:
            sendCommand("AT+QGNSSC=1", 1000);
            gpsEnabled = true;
            Serial.println("GPS enabled.");
            stateTimer = now;
            currentState = STATE_WAIT_GPS_FIX;
            break;

        case STATE_WAIT_GPS_FIX: {
            float lat, lon;
            if (getGPSCoordinates(lat, lon)) {
                Serial.println("Valid GPS fix acquired.");
                lastUploadTime = now;
                currentState = STATE_RUNNING;
            } else if (now - stateTimer > GPS_FIX_TIMEOUT_MS) {
                Serial.println("GPS fix timeout, continuing anyway.");
                currentState = STATE_RUNNING;
            } else {
                Serial.println("Waiting for valid GPS fix...");
            }
            delay(1000);
            break;
        }

        case STATE_RUNNING:
            // Sadece belirli aralıklarla Adafruit'e konum at
            if (gpsEnabled && now - lastUploadTime >= UPLOAD_INTERVAL_MS) {
                float lat, lon;
                if (getGPSCoordinates(lat, lon)) publishToAdafruitIO(lat, lon);
                lastUploadTime = now;
            }
            break;

        case STATE_FATAL_ERROR:
            Serial.println("FATAL ERROR! Halting.");
            delay(10000);
            break;
    }
}

// --- AT Command Helper ---
String sendCommand(String cmd, unsigned long timeout, bool print) {
    if (cmd.length() > 0) {
        MC60Serial.println(cmd);
        if(print) Serial.println(">> " + cmd);
    }

    unsigned long start = millis();
    String response = "";
    
    // Gelen veriyi körlemesine silmek yerine, global buffer'a da ekliyoruz.
    // Bu sayede komut beklerken araya giren SMS'ler silinmemiş oluyor.
    while(millis() - start < timeout) {
        while(MC60Serial.available()) {
            char c = MC60Serial.read();
            response += c;
            mc60Buffer += c; 
        }
    }
    if(print && response.length()>0) Serial.println("<< " + response);
    
    checkForSMS(); // Komut sonrasında buffer'ı kontrol et
    return response;
}

// --- Authorization Check ---
bool isAuthorized(String number) {
    if (number.length() == 0) return false;
    for (size_t i = 0; i < kAuthorizedCount; i++) {
        if (number.equals(kAuthorized[i])) return true;
    }
    return false;
}

// --- Asynchronous SMS Parsing (sender extraction added) ---
void checkForSMS() {
    // AT+CNMI=2,2 aktifken gelen SMS'ler "+CMT:" başlığı ile başlar
    int cmtIdx = mc60Buffer.indexOf("+CMT:");
    if (cmtIdx != -1) {
        int headerEnd = mc60Buffer.indexOf('\n', cmtIdx);
        if (headerEnd != -1) {
            int textEnd = mc60Buffer.indexOf('\n', headerEnd + 1);
            if (textEnd != -1) {
                // Göndereni başlıktan çıkart: +CMT: "+905551234567",...
                // İlk çift tırnak arasındaki numarayı al
                String sender = "";
                int q1 = mc60Buffer.indexOf('"', cmtIdx);
                if (q1 != -1 && q1 < headerEnd) {
                    int q2 = mc60Buffer.indexOf('"', q1 + 1);
                    if (q2 != -1 && q2 < headerEnd) {
                        sender = mc60Buffer.substring(q1 + 1, q2);
                    }
                }

                // Mesaj metnini çıkart
                String smsText = mc60Buffer.substring(headerEnd + 1, textEnd);
                smsText.trim();
                
                // KRİTİK NOKTA: Sonsuz döngüye girmemek için cevap vermeden ÖNCE hafızayı temizle!
                mc60Buffer.remove(0, textEnd + 1);

                Serial.println("\n[*** DIRECT SMS RECEIVED ***]");
                Serial.println("From: " + sender);
                Serial.println("Message: " + smsText);

                // Yetki kontrolü: listede olmayan numaralar sessizce yok sayılır
                if (!isAuthorized(sender)) {
                    Serial.println("Sender NOT authorized. Ignoring.");
                    return;
                }

                // Komut karşılaştırmasını "içeriyor mu" mantığıyla yap
                String cmd = smsText;
                cmd.toUpperCase();

                if (cmd.endsWith("PWROFF")) {
                    Serial.println("Action: Powering Off.");
                    sendSMS(sender, "Powering off.");
                    powerOffModule();
                } else if (cmd.endsWith("LOC") || cmd.endsWith("GPS") || cmd.endsWith("GETGPS")) {
                    Serial.println("Action: Fetching GPS and replying...");
                    sendGPSviaSMS(sender);
                } else if (cmd.endsWith("STATUS")) {
                    Serial.println("Action: Sending status...");
                    String msg = "State=" + String((int)currentState) + " Uptime=" + String(millis() / 1000) + "s";
                    sendSMS(sender, msg);
                } else {
                    sendSMS(sender, "Unknown command. Try: LOC, STATUS, PWROFF");
                }
            }
        }
    }

    // Sistemin hafızasını temiz tut (Gereksiz AT komut cevapları birikmesin)
    if (mc60Buffer.length() > 1000) {
        mc60Buffer = ""; 
    }
}

// --- Send SMS Helper ---
void sendSMS(String number, String text) {
    Serial.println("Sending SMS to: " + number);
    sendCommand("AT+CMGS=\"" + number + "\"", 2000, true);
    MC60Serial.print(text);
    delay(100);
    MC60Serial.write(26); // ASCII code for Ctrl+Z (SMS girişini bitir ve gönder)
    sendCommand("", 10000, true); // Şebekenin göndermesi için 10 saniye bekle
    Serial.println("SMS Sent.");
}

// --- Send GPS Location via SMS (replies to sender now) ---
void sendGPSviaSMS(String replyTo) {
    float lat, lon;
    if (getGPSCoordinates(lat, lon)) {
        // Tıklanabilir Google Haritalar linki oluştur
        String msg = "Location: https://maps.google.com/?q=" + String(lat,6) + "," + String(lon,6);
        sendSMS(replyTo, msg);
    } else {
        sendSMS(replyTo, "No GPS fix available right now. Try again later.");
    }
}

// --- Get GPS Coordinates ---
bool getGPSCoordinates(float &lat, float &lon) {
    String resp = sendCommand("AT+QGNSSRD=\"NMEA/GGA\"", 2000, false);
    int ggaIndex = resp.indexOf("$GNGGA");
    if(ggaIndex == -1) return false;

    int endLine = resp.indexOf('\n', ggaIndex);
    String gga = resp.substring(ggaIndex, endLine);

    char gga_cstr[gga.length()+1];
    strcpy(gga_cstr, gga.c_str());
    char* token = strtok(gga_cstr, ",");
    String fields[15];
    int count = 0;
    while(token && count<15) {
        fields[count++] = String(token);
        token = strtok(NULL,",");
    }

    if(count<7 || fields[6]=="0") return false; // GPS sinyali/fix yok

    float latVal = fields[2].toFloat();
    lat = floor(latVal/100) + fmod(latVal,100)/60.0;
    if(fields[3]=="S") lat=-lat;

    float lonVal = fields[4].toFloat();
    lon = floor(lonVal/100) + fmod(lonVal,100)/60.0;
    if(fields[5]=="W") lon=-lon;

    return true;
}

// --- Publish to Adafruit IO ---
void publishToAdafruitIO(float lat, float lon) {
    String feedPath = String(IO_USERNAME) + "/feeds/" + String(FEED_NAME);
    String payload = "{\"lat\":" + String(lat,6) + ",\"lon\":" + String(lon,6) + "}";
    String cmd = "AT+QMTPUB=0,0,0,0,\"" + feedPath + "\"";
    String resp = sendCommand(cmd,5000);
    if(resp.indexOf(">")!=-1) {
        MC60Serial.print(payload);
        delay(10);
        MC60Serial.write(26); // Ctrl+Z
        sendCommand("",10000); // Wait final OK
        Serial.println("Published to Adafruit IO!");
    } else Serial.println("Failed MQTT publish.");
}

// --- Power Off Module ---
void powerOffModule() {
    digitalWrite(MC60_PWRKEY_PIN, HIGH);
    delay(1200); 
    digitalWrite(MC60_PWRKEY_PIN, LOW);
    Serial.println("Module powered off.");
    currentState = STATE_POWER_ON; 
}