#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266SSDP.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPUpdateServer.h>
#include <DNSServer.h>
#include <TickerScheduler.h> // Библиотека тикета https://github.com/Toshik/TickerScheduler
// Библиотеки устройств
#include <DHT.h>  // https://github.com/markruys/arduino-DHT
#include <time.h> // http://esp8266-arduinoide.ru/step8-timeupdate/
#include <MQTT.h>
#include <SPI.h>
#include <GyverTM1637.h>
#include <GyverButton.h>
#include <Wire.h>
#include <BlynkSimpleEsp8266.h>

#define BLYNK_PRINT Serial
#define DHTPIN 14                                // Назначить пин датчика температуры
#define DHTTYPE DHT22                             // DHT 22, AM2302, AM2321
#define CLK 2						  // Назначить пин дисплея
#define DIO 3						  // Назначить пин дисплея
#define WIFI_SIGNAL_VPIN V80 // Пин уровня сигнала WiFi
#define BTN_M_PIN 0			  // Назначить кномпку меню и сброса параметров
#define BTN_UP_PIN 4				  // кнопка Up подключена сюда (BTN_PIN --- КНОПКА --- GND)
#define BTN_DOWN_PIN 5				  // кнопка Down подключена сюда (BTN_PIN --- КНОПКА --- GND)
#define PRESS_TIME_GYVER 5000		  // Время длинного нажатия кнопок Up/Down
#define mqtt_topic_temp "/sensors/dht/vagon/temp" // Топик температуры
#define mqtt_topic_hum "/sensors/dht/vagon/hum"			  // Топик влажности

// Объект для обнавления с web страницы
ESP8266HTTPUpdateServer httpUpdater;
// Web интерфейс для устройства
ESP8266WebServer HTTP(80);
// Инициализация MQTT
WiFiClient espClient;
MQTTClient mqttClient;
// Для файловой системы
File fsUploadFile;
// Для работы символьных имен в режиме AP
DNSServer dnsServer;
//Планировщик задач (Число задач)
TickerScheduler ts(2);
// Датчик DHT
DHT dht(DHTPIN, DHTTYPE); // Объявляем датчик температуры
//DHT dht;
GyverTM1637 disp(CLK, DIO);		 //Объявляем дисплей
GButton butt_Menu(BTN_M_PIN);	 //Объявляем кнопку Up
GButton butt_Up(BTN_UP_PIN);	 //Объявляем кнопку Up
GButton butt_Down(BTN_DOWN_PIN); //Объявляем кнопку Dovn
BlynkTimer timer;

bool dispTemp = true; // Переменная выбора отображения температуры/влажности
bool dispTempGist = false; // Переменная настройки гистарезиса температуры
bool dispHumGist = false; // Переменная настройки гистарезиса влажности
bool dispSave = true; // Переменная сохранения меню настроек
String configSetup = "{}"; // данные для config.setup.json
String configJson = "{}";  // данные для config.live.json

int mqttStatus; // Счетчик попыток подключения к MQTT
float TEMP_CORR;
// Запрос данных с MQTT
/* void callback(char *topic, byte *payload, unsigned int length)
{
	Serial.print("Message arrived [");
	Serial.print(topic); // отправляем в монитор порта название топика
	Serial.print("] ");
	for (int i = 0; i < length; i++)
	{ // отправляем данные из топика
		Serial.print((char)payload[i]);
	}
	Serial.println();
} */

void messageReceived(String &topic, String &payload)
{
  Serial.println("incoming: " + topic + " - " + payload);
}

void handleFileDelete()
{
  if (HTTP.args() == 0)
    return HTTP.send(500, "text/plain", "BAD ARGS");
  String path = HTTP.arg(0);
  if (path == "/")
    return HTTP.send(500, "text/plain", "BAD PATH");
  if (!SPIFFS.exists(path))
    return HTTP.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  HTTP.send(200, "text/plain", "");
  path = String();
}

void handleFileCreate()
{
  if (HTTP.args() == 0)
    return HTTP.send(500, "text/plain", "BAD ARGS");
  String path = HTTP.arg(0);
  if (path == "/")
    return HTTP.send(500, "text/plain", "BAD PATH");
  if (SPIFFS.exists(path))
    return HTTP.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if (file)
    file.close();
  else
    return HTTP.send(500, "text/plain", "CREATE FAILED");
  HTTP.send(200, "text/plain", "");
  path = String();
}

void handleFileList()
{
  if (!HTTP.hasArg("dir"))
  {
    HTTP.send(500, "text/plain", "BAD ARGS");
    return;
  }
  String path = HTTP.arg("dir");
  Dir dir = SPIFFS.openDir(path);
  path = String();
  String output = "[";
  while (dir.next())
  {
    File entry = dir.openFile("r");
    if (output != "[")
      output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir) ? "dir" : "file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }
  output += "]";
  HTTP.send(200, "text/json", output);
}

void handleFileUpload()
{
  if (HTTP.uri() != "/edit")
    return;
  HTTPUpload &upload = HTTP.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    String filename = upload.filename;
    if (!filename.startsWith("/"))
      filename = "/" + filename;
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    //DBG_OUTPUT_PORT.print("handleFileUpload Data: "); DBG_OUTPUT_PORT.println(upload.currentSize);
    if (fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (fsUploadFile)
      fsUploadFile.close();
  }
}

// Здесь функции для работы с файловой системой
String getContentType(String filename)
{
  if (HTTP.hasArg("download"))
    return "application/octet-stream";
  else if (filename.endsWith(".htm"))
    return "text/html";
  else if (filename.endsWith(".html"))
    return "text/html";
  else if (filename.endsWith(".json"))
    return "application/json";
  else if (filename.endsWith(".css"))
    return "text/css";
  else if (filename.endsWith(".js"))
    return "application/javascript";
  else if (filename.endsWith(".png"))
    return "image/png";
  else if (filename.endsWith(".gif"))
    return "image/gif";
  else if (filename.endsWith(".jpg"))
    return "image/jpeg";
  else if (filename.endsWith(".ico"))
    return "image/x-icon";
  else if (filename.endsWith(".xml"))
    return "text/xml";
  else if (filename.endsWith(".pdf"))
    return "application/x-pdf";
  else if (filename.endsWith(".zip"))
    return "application/x-zip";
  else if (filename.endsWith(".gz"))
    return "application/x-gzip";
  return "text/plain";
}

bool handleFileRead(String path)
{
  if (path.endsWith("/"))
    path += "index.htm";
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path))
  {
    if (SPIFFS.exists(pathWithGz))
      path += ".gz";
    File file = SPIFFS.open(path, "r");
    size_t sent = HTTP.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

// Инициализация FFS
void FS_init(void)
{
  SPIFFS.begin();
  {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next())
    {
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
    }
  }
  //HTTP страницы для работы с FFS
  //list directory
  HTTP.on("/list", HTTP_GET, handleFileList);
  //загрузка редактора editor
  HTTP.on("/edit", HTTP_GET, []() {
    if (!handleFileRead("/edit.htm"))
      HTTP.send(404, "text/plain", "FileNotFound");
  });
  //Создание файла
  HTTP.on("/edit", HTTP_PUT, handleFileCreate);
  //Удаление файла
  HTTP.on("/edit", HTTP_DELETE, handleFileDelete);
  //first callback is called after the request has ended with all parsed arguments
  //second callback handles file uploads at that location
  HTTP.on("/edit", HTTP_POST, []() {
    HTTP.send(200, "text/plain", "");
  },
          handleFileUpload);
  //called when the url is not defined here
  //use it to load content from SPIFFS
  HTTP.onNotFound([]() {
    if (!handleFileRead(HTTP.uri()))
      HTTP.send(404, "text/plain", "FileNotFound");
  });
}

/* Настройка Json */
// ------------- Чтение значения json
String jsonRead(String &json, String name)
{
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.parseObject(json);
  return root[name].as<String>();
}
char jsonReadChar(String &json, String name)
{
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.parseObject(json);
  return root[name].as<char>();
}
// ------------- Чтение значения json
int jsonReadtoInt(String &json, String name)
{
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.parseObject(json);
  return root[name];
}

uint16_t jsonReadtoUint(String &json, String name)
{
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.parseObject(json);
  return root[name].as<u16_t>();
}

String getSetup(String Name)
{
  return jsonRead(configSetup, Name);
}
// ------------- Запись значения json String
String jsonWrite(String &json, String name, String volume)
{
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.parseObject(json);
  root[name] = volume;
  json = "";
  root.printTo(json);
  return json;
}
// ------------- Запись значения json int
String jsonWrite(String &json, String name, int volume)
{
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.parseObject(json);
  root[name] = volume;
  json = "";
  root.printTo(json);
  return json;
}
// ------------- Запись значения json float
String jsonWrite(String &json, String name, float volume)
{
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.parseObject(json);
  root[name] = volume;
  json = "";
  root.printTo(json);
  return json;
}
String writeFile(String fileName, String strings)
{
  File configFile = SPIFFS.open("/" + fileName, "w");
  if (!configFile)
  {
    return "Failed to open config file";
  }
  configFile.print(strings);
  //strings.printTo(configFile);
  configFile.close();
  return "Write sucsses";
}
void saveConfig()
{
  writeFile("config.json", configSetup);
}
// ------------- Чтение файла в строку
String readFile(String fileName, size_t len)
{
  File configFile = SPIFFS.open("/" + fileName, "r");
  if (!configFile)
  {
    return "Failed";
  }
  size_t size = configFile.size();
  if (size > len)
  {
    configFile.close();
    return "Large";
  }
  String temp = configFile.readString();
  configFile.close();
  return temp;
}
// ------------- Запись строки в файл
// Перегрузка функций
// ------------- Создание данных для графика
String graf(int datas)
{
  String root = "{}"; // Формировать строку для отправки в браузер json формат
  // {"data":[1]}
  // Резервируем память для json обекта буфер может рости по мере необходимти, предпочтительно для ESP8266
  DynamicJsonBuffer jsonBuffer;
  // вызовите парсер JSON через экземпляр jsonBuffer
  JsonObject &json = jsonBuffer.parseObject(root);
  // Заполняем поля json
  JsonArray &data = json.createNestedArray("data");
  data.add(datas);
  // Помещаем созданный json в переменную root
  root = "";
  json.printTo(root);
  return root;
}
// ------------- Создание данных для графика
String graf(float datas)
{
  String root = "{}"; // Формировать строку для отправки в браузер json формат
  // {"data":[1]}
  // Резервируем память для json обекта буфер может рости по мере необходимти, предпочтительно для ESP8266
  DynamicJsonBuffer jsonBuffer;
  // вызовите парсер JSON через экземпляр jsonBuffer
  JsonObject &json = jsonBuffer.parseObject(root);
  // Заполняем поля json
  JsonArray &data = json.createNestedArray("data");
  data.add(datas);
  // Помещаем созданный json в переменную root
  root = "";
  json.printTo(root);
  return root;
}
// ------------- Создание данных для графика
String graf(int datas, int datas1)
{
  String root = "{}"; // Формировать строку для отправки в браузер json формат
  // {"data":[1]}
  // Резервируем память для json обекта буфер может рости по мере необходимти, предпочтительно для ESP8266
  DynamicJsonBuffer jsonBuffer;
  // вызовите парсер JSON через экземпляр jsonBuffer
  JsonObject &json = jsonBuffer.parseObject(root);
  // Заполняем поля json
  JsonArray &data = json.createNestedArray("data");
  JsonArray &data1 = json.createNestedArray("data1");
  data.add(datas);
  data1.add(datas1);
  // Помещаем созданный json в переменную root
  root = "";
  json.printTo(root);
  return root;
}
// ------------- Создание данных для графика
String graf(float datas, float datas1)
{
  String root = "{}"; // Формировать строку для отправки в браузер json формат
  // {"data":[1]}
  // Резервируем память для json обекта буфер может рости по мере необходимти, предпочтительно для ESP8266
  DynamicJsonBuffer jsonBuffer;
  // вызовите парсер JSON через экземпляр jsonBuffer
  JsonObject &json = jsonBuffer.parseObject(root);
  // Заполняем поля json
  JsonArray &data = json.createNestedArray("data");
  JsonArray &data1 = json.createNestedArray("data1");
  data.add(datas);
  data1.add(datas1);
  // Помещаем созданный json в переменную root
  root = "";
  json.printTo(root);
  return root;
}
// ------------- Создание данных для графика
String graf(float datas, float datas1, float datas2)
{
  String root = "{}"; // Формировать строку для отправки в браузер json формат
  // {"data":[1]}
  // Резервируем память для json обекта буфер может рости по мере необходимти, предпочтительно для ESP8266
  DynamicJsonBuffer jsonBuffer;
  // вызовите парсер JSON через экземпляр jsonBuffer
  JsonObject &json = jsonBuffer.parseObject(root);
  // Заполняем поля json
  JsonArray &data = json.createNestedArray("data");
  JsonArray &data1 = json.createNestedArray("data1");
  JsonArray &data2 = json.createNestedArray("data2");
  data.add(datas);
  data1.add(datas1);
  data2.add(datas2);
  // Помещаем созданный json в переменную root
  root = "";
  json.printTo(root);
  return root;
}
// ------------- Создание данных для графика
String graf(int datas, int datas1, int datas2)
{
  String root = "{}"; // Формировать строку для отправки в браузер json формат
  // {"data":[1]}
  // Резервируем память для json обекта буфер может рости по мере необходимти, предпочтительно для ESP8266
  DynamicJsonBuffer jsonBuffer;
  // вызовите парсер JSON через экземпляр jsonBuffer
  JsonObject &json = jsonBuffer.parseObject(root);
  // Заполняем поля json
  JsonArray &data = json.createNestedArray("data");
  JsonArray &data1 = json.createNestedArray("data1");
  JsonArray &data2 = json.createNestedArray("data2");
  data.add(datas);
  data1.add(datas1);
  data2.add(datas2);
  // Помещаем созданный json в переменную root
  root = "";
  json.printTo(root);
  return root;
}
/* Конец настройки Json */

bool StartAPMode()
{
  IPAddress apIP(192, 168, 4, 1);
  IPAddress staticGateway(192, 168, 4, 1);
  IPAddress staticSubnet(255, 255, 255, 0);
  // Отключаем WIFI
  WiFi.disconnect();
  // Меняем режим на режим точки доступа
  WiFi.mode(WIFI_AP);
  // Задаем настройки сети
  WiFi.softAPConfig(apIP, staticGateway, staticSubnet);
  //Включаем DNS
  dnsServer.start(53, "*", apIP);
  // Включаем WIFI в режиме точки доступа с именем и паролем
  // хронящихся в переменных _ssidAP _passwordAP
  String _ssidAP = jsonRead(configSetup, "ssidAP");
  String _passwordAP = jsonRead(configSetup, "passwordAP");
  WiFi.softAP(_ssidAP.c_str(), _passwordAP.c_str());
  return true;
}

void connect()
{

  while (!mqttClient.connect(jsonRead(configSetup, "SSDP").c_str(), jsonRead(configSetup, "mqttLogin").c_str(), jsonRead(configSetup, "mqttPassword").c_str()) && mqttStatus < 10)
  {
    mqttStatus++;
    Serial.print(mqttStatus);
    delay(1000);
  }

  Serial.println("\nconnected!");
  Serial.println(jsonRead(configSetup, "mqttServer"));

  mqttClient.subscribe(mqtt_topic_temp);
  // client.unsubscribe("/hello");
}

/* void reconnect()
{
	while (!mqttClient.connected())
	{ // крутимся пока не подключемся.
		// подключаемся, в client.connect передаем ID, логин и пасс
		if (mqttClient.connect("SSDP", jsonRead(configSetup, "mqttLogin").c_str(), jsonRead(configSetup, "mqttPassword").c_str()))
		{
			mqttClient.subscribe(mqtt_topic_temp);	 // подписываемся на топик, в который же пишем данные

		}
		else
		{
			Serial.println("dfhfgjjjjjjjjjjjjjjjjjfjjj");
		}
	}
} */

// TODO: Инициализация WiFi
void WIFIinit()
{
  // --------------------Получаем SSDP со страницы
  HTTP.on("/ssid", HTTP_GET, []() {
    jsonWrite(configSetup, "ssid", HTTP.arg("ssid"));
    jsonWrite(configSetup, "password", HTTP.arg("password"));
    saveConfig();                       // Функция сохранения данных во Flash
    HTTP.send(200, "text/plain", "OK"); // отправляем ответ о выполнении
  });
  // --------------------Получаем SSDP со страницы
  HTTP.on("/ssidap", HTTP_GET, []() {
    jsonWrite(configSetup, "ssidAP", HTTP.arg("ssidAP"));
    jsonWrite(configSetup, "passwordAP", HTTP.arg("passwordAP"));
    saveConfig();                       // Функция сохранения данных во Flash
    HTTP.send(200, "text/plain", "OK"); // отправляем ответ о выполнении
  });

  // --------------------Получаем MQTT со страницы
  HTTP.on("/mqtt", HTTP_GET, []() {
    jsonWrite(configSetup, "mqttServer", HTTP.arg("mqttServer"));
    jsonWrite(configSetup, "mqttPort", HTTP.arg("mqttPort"));
    jsonWrite(configSetup, "mqttLogin", HTTP.arg("mqttLogin"));
    jsonWrite(configSetup, "mqttPassword", HTTP.arg("mqttPassword"));
    saveConfig();                       // Функция сохранения данных во Flash
    HTTP.send(200, "text/plain", "OK"); // отправляем ответ о выполнении
  });

  // --------------------Получаем параметры Blynk со страницы
  HTTP.on("/blynk", HTTP_GET, []() {
    jsonWrite(configSetup, "blynkServer", HTTP.arg("blynkServer"));
    jsonWrite(configSetup, "blynkPort", HTTP.arg("blynkPort"));
    jsonWrite(configSetup, "blynkToken", HTTP.arg("blynkToken"));
    saveConfig();                       // Функция сохранения данных во Flash
    HTTP.send(200, "text/plain", "OK"); // отправляем ответ о выполнении
  });

    HTTP.on("/temp_corr", HTTP_GET, []() { // TODO: Получаем коррекцию температуры со страницы
    jsonWrite(configSetup, "temp_corr", HTTP.arg("temp_corr"));
    saveConfig();                       // Функция сохранения данных во Flash
    HTTP.send(200, "text/plain", "OK"); // отправляем ответ о выполнении
  });

  // Попытка подключения к точке доступа
  WiFi.mode(WIFI_STA);
  byte tries = 11;
  String _ssid = jsonRead(configSetup, "ssid");
  String _password = jsonRead(configSetup, "password");
  if (_ssid == "" && _password == "")
  {
    WiFi.begin();
  }
  else
  {
    WiFi.begin(_ssid.c_str(), _password.c_str());
  }
  // Делаем проверку подключения до тех пор пока счетчик tries
  // не станет равен нулю или не получим подключение
  while (--tries && WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(1000);
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    // Если не удалось подключиться запускаем в режиме AP
    Serial.println("");
    Serial.println("WiFi up AP");
    StartAPMode();
  }
  else
  {
    // Иначе удалось подключиться отправляем сообщение
    // о подключении и выводим адрес IP
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
    connect();
  }
}

// Получение текущего времени
String GetTime()
{
  time_t now = time(nullptr);          // получаем время с помощью библиотеки time.h
  String Time = "";                    // Строка для результатов времени
  Time += ctime(&now);                 // Преобразуем время в строку формата Thu Jan 19 00:55:35 2017
  int i = Time.indexOf(":");           //Ишем позицию первого символа :
  Time = Time.substring(i - 2, i + 6); // Выделяем из строки 2 символа перед символом : и 6 символов после
  return Time;                         // Возврашаем полученное время
}
// Получение даты
String GetDate()
{
  time_t now = time(nullptr);                 // получаем время с помощью библиотеки time.h
  String Data = "";                           // Строка для результатов времени
  Data += ctime(&now);                        // Преобразуем время в строку формата Thu Jan 19 00:55:35 2017
  int i = Data.lastIndexOf(" ");              //Ишем позицию последнего символа пробел
  String Time = Data.substring(i - 8, i + 1); // Выделяем время и пробел
  Data.replace(Time, "");                     // Удаляем из строки 8 символов времени и пробел
  Data.replace("\n", "");                     // Удаляем символ переноса строки
  return Data;                                // Возврашаем полученную дату
}

void timeSynch(int zone)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    // Настройка соединения с NTP сервером
    configTime(zone * 3600, 0, "pool.ntp.org", "ru.pool.ntp.org");
    int i = 0;
    Serial.println("\nWaiting for time");
    while (!time(nullptr) && i < 10)
    {
      Serial.print(".");
      i++;
      delay(1000);
    }
    Serial.println("");
    Serial.println("ITime Ready!");
    Serial.println(GetTime());
    Serial.println(GetDate());
  }
}

void handle_Time()
{
  timeSynch(jsonReadtoInt(configSetup, "timezone"));
  HTTP.send(200, "text/plain", "OK"); // отправляем ответ о выполнении
}

// Установка параметров времянной зоны по запросу вида http://192.168.0.101/timeZone?timeZone=3
void handle_time_zone()
{
  jsonWrite(configSetup, "timezone", (int)HTTP.arg("timeZone").toInt()); // Получаем значение timezone из запроса конвертируем в int сохраняем
  saveConfig();
  HTTP.send(200, "text/plain", "OK");
}

void Time_init()
{
  HTTP.on("/Time", handle_Time);          // Синхронизировать время устройства по запросу вида /Time
  HTTP.on("/timeZone", handle_time_zone); // Установка времянной зоны по запросу вида http://192.168.0.101/timeZone?timeZone=3
  timeSynch(jsonReadtoInt(configSetup, "timezone"));
}

// -----------------  DHT
void DHT_init()
{
  dht.begin();                            //Запускаем датчик
  //dht.setup(DHTPIN);                            //Запускаем датчик
  delay(1000);                            // Нужно ждать иначе датчик не определится правильно
  static uint16_t test = 1000;            // Получим минимальное время между запросами данных с датчика
  jsonWrite(configJson, "dhttime", test); // Отправим в json переменную configJson ключ dhttime полученное значение
  dht.readTemperature();                  // обязательно делаем пустое чтение первый раз иначе чтение статуса не сработает
  //dht.getTemperature();                  // обязательно делаем пустое чтение первый раз иначе чтение статуса не сработает
  bool statusDHT = dht.read();            // Определим стстус датчика
  Serial.print("DHT = ");
  Serial.println(statusDHT);                    //  и сообщим в Serial
TEMP_CORR = jsonReadtoInt(configSetup, "temp_corr");
                                                           // включим задачу если датчик есть
    jsonWrite(configJson, "temperature", (dht.readTemperature() + TEMP_CORR)); // отправить температуру в configJson
    jsonWrite(configJson, "humidity", dht.readHumidity());       // отправить влажность в configJson
    //jsonWrite(configJson, "temperature", dht.getTemperature()); // отправить температуру в configJson
    //jsonWrite(configJson, "humidity", dht.getHumidity());       // отправить влажность в configJson
 

    ts.add(0, 5000, [&](void *) {                                  // Запустим задачу 0 с интервалом test
		char msgT[10];
		char msgH[10];
		//float tm = dht.getTemperature();
		//float hm = dht.getHumidity();
    float tm = dht.readTemperature() + TEMP_CORR;
		float hm = dht.readHumidity();
		if (dispTemp && dispSave)
		{
			disp.displayInt(round(tm * 10) / 10); // Убираем дробную часть
			disp.displayByte(0, _C);			 // Вывод символа C
		}
		else if (!dispTemp && dispSave)
		{
			disp.displayInt(round(hm * 10) / 10); // Убираем дробную часть
			disp.displayByte(0, _H);			 // Вывод символа H
		}
    Blynk.virtualWrite(V1, tm);
		Blynk.virtualWrite(V2, hm);
		dtostrf(tm, 5, 1, msgT);
		mqttClient.publish(mqtt_topic_temp, msgT); // пишем в топик
		dtostrf(hm, 5, 0, msgH);
		mqttClient.publish(mqtt_topic_hum, msgH); // пишем в топик
      jsonWrite(configJson, "temperature", tm); // отправить температуру в configJson
      jsonWrite(configJson, "humidity", hm);       // отправить влажность в configJson
      //mqttClient.publish(mqtt_topic_temp, "777");                  // пишем в топик
      Serial.print(".");
    },
           nullptr, true);
}

// -----------------  Вывод времени и даты в /config.live.json каждую секунду
void sec_init()
{
  ts.add(1, 1000, [&](void *) { // Запустим задачу 1 с интервалом 1000ms
    // поместим данные для web страницы в json строку configJson
    // Будем вызывать эту функцию каждый раз при запросе /config.live.json
    // jsonWrite(строка, "ключ", значение_число); Так можно дабавить или обнавить json значение ключа в строке
    // jsonWrite(строка, "ключ", "значение_текст");
    jsonWrite(configJson, "time", GetTime()); // отправить время в configJson
    jsonWrite(configJson, "date", GetDate()); // отправить дату в configJson
    Blynk.virtualWrite(WIFI_SIGNAL_VPIN, map(WiFi.RSSI(), -105, -40, 0, 100)); // Получаем уровень сигнала Wifi
  },
         nullptr, true);
}
/* ---------------- Задание для закрепления материала
 *  Заставьте мигать светодиод на любом pin с частотой 5 секунд
 *  сделайте новую задачу под индексом 3
 *  в /config.live.json отправляйте состояние светодиода с ключем "stateLed"
 *  Выводите состояние светодиода на график по запросу вида /charts.json?data=stateLed
 *  Процедуру blink_init() инициализируйте в setup
 */

void blink_init()
{
  // здесь пишите код решения
}

void SSDP_init(void)
{
  String chipID = String(ESP.getChipId()) + "-" + String(ESP.getFlashChipId());
  // SSDP дескриптор
  HTTP.on("/description.xml", HTTP_GET, []() {
    SSDP.schema(HTTP.client());
  });
  // --------------------Получаем SSDP со страницы
  HTTP.on("/ssdp", HTTP_GET, []() {
    String ssdp = HTTP.arg("ssdp");
    configJson = jsonWrite(configJson, "SSDP", ssdp);
    configJson = jsonWrite(configSetup, "SSDP", ssdp);
    SSDP.setName(jsonRead(configSetup, "SSDP"));
    saveConfig();                       // Функция сохранения данных во Flash
    HTTP.send(200, "text/plain", "OK"); // отправляем ответ о выполнении
  });
  //Если версия  2.0.0 закаментируйте следующую строчку
  SSDP.setDeviceType("upnp:rootdevice");
  SSDP.setSchemaURL("description.xml");
  SSDP.setHTTPPort(80);
  SSDP.setName(jsonRead(configSetup, "SSDP"));
  SSDP.setSerialNumber(chipID);
  SSDP.setURL("/");
  SSDP.setModelName("tech");
  SSDP.setModelNumber(chipID + "/" + jsonRead(configSetup, "SSDP"));
  SSDP.setModelURL("http://esp8266-arduinoide.ru/step12-graf-dht/");
  SSDP.setManufacturer("Tretyakov Sergey");
  SSDP.setManufacturerURL("http://www.esp8266-arduinoide.ru");
  SSDP.begin();
}

void HTTP_init(void)
{
  jsonWrite(configJson, "flashChip", String(ESP.getFlashChipId(), HEX));

  // -------------------построение графика по запросу вида /charts.json?data=A0&data2=stateLed
  HTTP.on("/charts.json", HTTP_GET, []() {
    String message = "{";    // создадим json на лету
    uint8_t j = HTTP.args(); // получим количество аргументов
    for (uint8_t i = 0; i < j; i++)
    {                                   // Будем читать все аргументы по порядку
      String nameArg = HTTP.argName(i); // Возьмем имя аргумента и зададим массив с ключём по имени аргумента
      String keyArg = HTTP.arg(i);
      String value = jsonRead(configJson, HTTP.arg(i)); // Считаем из configJson значение с ключём keyArg
      if (value != "")
      {                                     // если значение есть добавим имя массива
        message += "\"" + nameArg + "\":["; // теперь в строке {"Имя аргумента":[
        message += value;                   // добавим данные в массив теперь в строке {"Имя аргумента":[value
        value = "";                         // очистим value
      }
      message += "]"; // завершим массив теперь в строке {"Имя аргумента":[value]
      if (i < j - 1)
        message += ","; // если элемент не последний добавит , теперь в строке {"Имя аргумента":[value],
    }
    message += "}";
    // теперь json строка полная
    jsonWrite(message, "points", 10);    // зададим количество точек по умолчанию
    jsonWrite(message, "refresh", 1000); // зададим время обнавления графика по умолчанию
    HTTP.send(200, "application/json", message);
  });

  // --------------------Выдаем данные configJson
  HTTP.on("/config.live.json", HTTP_GET, []() {
    HTTP.send(200, "application/json", configJson);
  });
  // -------------------Выдаем данные configSetup
  HTTP.on("/config.setup.json", HTTP_GET, []() {
    HTTP.send(200, "application/json", configSetup);
  });
  // -------------------Перезагрузка модуля
  HTTP.on("/restart", HTTP_GET, []() {
    String restart = HTTP.arg("device"); // Получаем значение device из запроса
    if (restart == "ok")
    {                                             // Если значение равно Ок
      HTTP.send(200, "text / plain", "Reset OK"); // Oтправляем ответ Reset OK
      ESP.restart();                              // перезагружаем модуль
    }
    else
    {                                             // иначе
      HTTP.send(200, "text / plain", "No Reset"); // Oтправляем ответ No Reset
    }
  });
  // Добавляем функцию Update для перезаписи прошивки по WiFi при 1М(256K SPIFFS) и выше
  httpUpdater.setup(&HTTP);
  #include <ESP8266httpUpdate.h>
  // Запускаем HTTP сервер
  HTTP.begin();
}

void readButton()
{
    butt_Menu.tick();
  butt_Up.tick();
  butt_Down.tick();
  if (butt_Menu.isClick()) { // Одинарное нажатие кнопки Menu переключение температуры/влажности
    dispTempGist = false;
    dispHumGist = false;
    	float t = dht.readTemperature() + TEMP_CORR;
			float h = dht.readHumidity();
      //float t = dht.getTemperature();
			//float h = dht.getHumidity();
			if (dispTemp && dispSave)
			{
				dispTemp = false;
				disp.displayInt(round(h * 10) / 10); // Убираем дробную часть
				disp.displayByte(0, _H);			 // Вывод символа H
			}
			else if (!dispTemp && dispSave)
			{
				dispTemp = true;
				disp.displayInt(round(t * 10) / 10); // Убираем дробную часть
				disp.displayByte(0, _C);			 // Вывод символа C
			}
      else if (!dispSave) {
        dispSave = true;
      }
  }

    if (butt_Menu.isDouble()) { // Двойное нажатие кнопки Menu перезагрузка модуля
    disp.displayByte(_r, _E, _b, _t);
    mqttClient.disconnect();
    delay(2000);
    ESP.restart();
  }

  if (butt_Menu.isStep(1)) { // Нажатие и удержание кнопки Menu перезагрузка модуля
  disp.displayByte(_r, _E, _S, _t);
    mqttClient.disconnect();
    jsonWrite(configSetup, "ssid", "");
    jsonWrite(configSetup, "password", "");
    jsonWrite(configSetup, "mqttServer", "");
    jsonWrite(configSetup, "mqttPort", "");
    jsonWrite(configSetup, "mqttLogin", "");
    jsonWrite(configSetup, "mqttPassword", "");
    delay(2000);
    ESP.restart();
}

    if (butt_Up.isClick()) { // Длинное нажатие кнопки Up установка гистерезиса температуры
    //disp.clear();
				disp.displayByte(0, _H);			 // Вывод символа C
  }
}

void setup()
{
  Serial.begin(115200);
  delay(5);
  Serial.println("");
  //Запускаем файловую систему
  Serial.println("Start 4-FS");
  FS_init();
  Serial.println("Step7-FileConfig");
  configSetup = readFile("config.json", 4096);
  jsonWrite(configJson, "SSDP", jsonRead(configSetup, "SSDP"));
  Serial.println(configSetup);
  Serial.println("Start 1-WIFI");
  mqttStatus = 0;
  mqttClient.begin(jsonRead(configSetup, "mqttServer").c_str(), jsonReadtoInt(configSetup, "mqttPort"), espClient);
  mqttClient.onMessage(messageReceived);
  //Запускаем WIFI
  WIFIinit();

  Serial.println("Start 8-Time");
  // Получаем время из сети
  Time_init();
  sec_init();
  //Настраиваем и запускаем SSDP интерфейс
  Serial.println("Start 3-SSDP");
  SSDP_init();
  //Настраиваем и запускаем HTTP интерфейс
  Serial.println("Start 2-WebServer");
  HTTP_init();
  //	mqttClient.setServer(jsonRead(configSetup, "mqttServer").c_str(), jsonReadtoInt(configSetup, "mqttPort")); // указываем адрес брокера и порт
  //mqttClient.setCallback(callback);			  // указываем функцию которая вызывается когда приходят данные от брокера

  
  DHT_init();
  disp.clear();
	disp.brightness(7);						  // яркость, 0 - 7 (минимум - максимум)
	disp.point(0);							  // Отключить точки на дисплее
  butt_Menu.setTimeout(PRESS_TIME_GYVER);        // настройка таймаута на удержание (по умолчанию 500 мс)
  char blToken[50];
  strcpy(blToken, jsonRead(configSetup, "blynkToken").c_str());
  Blynk.config(blToken);
	if (Blynk.connect())
	{
		// something to do if connected
    Serial.println("Start 2222");
	}
	else
	{
		// something to do if you failed to connect
    Serial.println("Start 7777");
	}

}

void loop()
{
  	
  ts.update();         //планировщик задач
  HTTP.handleClient(); // Работа Web сервера
  yield();
  dnsServer.processNextRequest(); // Для работы DNS в режиме AP
                                  /*   	if (!mqttClient.connected())
	{				 // проверяем подключение к брокеру
     Serial.println(jsonRead(configSetup, "mqttServer"));
		reconnect(); // еще бы проверить подкючение к wifi...

	} */
  Blynk.run(); // Инициализация сервера Blynk
  if (!mqttClient.connected() && mqttStatus < 10)
  {
    connect();
  }
  mqttClient.loop();

  readButton();

}
