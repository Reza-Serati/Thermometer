/*****************************************************************************************************************************
 *  This sketch uses Arduino Nano board to get temperature value from 3 sensors (DALLAS 18B20) and show the average on LCD.
 *  This also checks for each sensor correctness and shows incorrect sensors on the LCD. In addition, it specifies simcard status
 *  on the bottom of LCD. Nano has serial communication with ESP8266. It sends temperature value and each sensor correctness to
 *  ESP, and receives simcard status from ESP.
 *****************************************************************************************************************************/


#include "U8glib.h"
#include <SoftwareSerial.h>
#include <OneWire.h>
#include <DallasTemperature.h>

//input pins comming from data pin of sensors
#define ONE_WIRE_BUS_PIN_1  A4
#define ONE_WIRE_BUS_PIN_2  A5
#define ONE_WIRE_BUS_PIN_3  A2

OneWire oneWire_1(ONE_WIRE_BUS_PIN_1);
OneWire oneWire_2(ONE_WIRE_BUS_PIN_2);
OneWire oneWire_3(ONE_WIRE_BUS_PIN_3);

DallasTemperature sensors_1(&oneWire_1);
DallasTemperature sensors_2(&oneWire_2);
DallasTemperature sensors_3(&oneWire_3);

DeviceAddress Probe_01 = {0x28, 0xFF, 0x2F, 0xAA, 0x60, 0x18, 0x02, 0xEE};
DeviceAddress Probe_02 = {0x28, 0xFF, 0x45, 0xDC, 0x67, 0x18, 0x01, 0x7B};
DeviceAddress Probe_03 = {0x28, 0xFF, 0xE1, 0x36, 0x68, 0x18, 0x01, 0x5A};

SoftwareSerial nano_esp_serial (A1, A3);      //Nano A1 &  A3       are connected to Esp8266     D3 & D4

#define nano_to_esp_baudrate    2400
#define nano_serial_baudrate      2400

//LCD
#define max_x_len     128
#define max_y_len     64
#define border_len    1

int16_t tempValue = 0;

#define max_safety_count   2 //used to ensure sensor failure
uint8_t sensor_no_1_safety_count = 0;
uint8_t sensor_no_2_safety_count = 0;
uint8_t sensor_no_3_safety_count = 0;

uint8_t valid_sensors = 0;

//these booleans show sensors correctness(true) or incorrectness(false)
boolean sensor_1_flag = false;
boolean sensor_2_flag = false;
boolean sensor_3_flag = false;

int16_t tempC_1; //temperature provided by sensor 1
int16_t tempC_2; //temperature provided by sensor 2
int16_t tempC_3; //temperature provided by sensor 3

  //backups are kept for checking sensor mistakes. they are updated every 28 seconds.
int16_t tempC_1_backup;
int16_t tempC_2_backup;
int16_t tempC_3_backup;

boolean sim_available = false;
uint8_t counter = 0; //we have a 4 seconds timer, and also we want to do something every 28 seconds. so we use a counter incrementation in 4 seconds timer ISR.

/*
   NANO
*/
U8GLIB_ST7920_128X64_4X u8g(9, 8, 7, 6, 5, 4, 3, 2,   10, 12, 11);
// 8Bit Com: D0..D7:pin7-14  -> 9,8,7,6,5,4,3,2       en=pin6=10, rs=pin4=12, rw==pin5=11

void calculate_temperature() {
  valid_sensors = 0;
  sensors_1.requestTemperatures();
  sensors_2.requestTemperatures();
  sensors_3.requestTemperatures();
  tempC_1 = sensors_1.getTempC(Probe_01);
  tempC_2 = sensors_2.getTempC(Probe_02);
  tempC_3 = sensors_3.getTempC(Probe_03);

  tempValue = 0;
  if (tempC_1 == -127) { //error
    sensor_no_1_safety_count++;

    if (sensor_no_1_safety_count == max_safety_count) { //we ensured failure of sensor 1
      sensor_1_flag = false;
      handle_temperature_fault(1);
    }
  }
  //if current value is 85 and differs more than 10 degree from backup value, there is an error in this sensor.
  //we checked for difference, because 85 is in working range of sensors. (means that temperature may really be 85, without any fault)
  else if (tempC_1 == 85 && (tempC_1_backup < 75 || tempC_1_backup > 95)) {
    sensor_1_flag = false;
    tempC_1_backup = 150; //set to 150 to be out of working range of sensors.
    handle_temperature_fault(1);
  }
  else {
    sensor_no_1_safety_count = 0;
    sensor_1_flag = true;
    tempValue += tempC_1;
    valid_sensors++;
  }
  //repeat for sensor 2 and 3
  if (tempC_2 == -127) {
    sensor_no_2_safety_count++;

    if (sensor_no_2_safety_count == max_safety_count) {
      sensor_2_flag = false;
      handle_temperature_fault(2);
    }
  }
  else if (tempC_2 == 85 && (tempC_2_backup < 75 || tempC_2_backup > 95)) {
    sensor_2_flag = false;
    tempC_2_backup = 150;
    handle_temperature_fault(2);
  }
  else {
    sensor_no_2_safety_count = 0;
    sensor_2_flag = true;
    tempValue += tempC_2;
    valid_sensors++;
  }
  if (tempC_3 == -127) {
    sensor_no_3_safety_count++;

    if (sensor_no_3_safety_count == max_safety_count) {
      sensor_3_flag = false;
      handle_temperature_fault(3);
    }
  }
  else if (tempC_3 == 85 && (tempC_3_backup < 75 || tempC_3_backup > 95)) {
    sensor_3_flag = false;
    tempC_3_backup = 150;
    handle_temperature_fault(3);
  }
  else {
    sensor_no_3_safety_count = 0;
    sensor_3_flag = true;
    tempValue += tempC_3;
    valid_sensors++;
  }
  tempValue = tempValue / valid_sensors;
}

//initialize timer 1 to tick every 4 seconds.
void initTimer4s() {
  cli(); // stop interrupts
  TCCR1A = 0; // set entire TCCR1A register to 0
  TCCR1B = 0; // same for TCCR1B
  TCNT1  = 0; // initialize counter value to 0
  // set compare match register for 0.25 Hz increments
  OCR1A = 62499; // = 16000000 / (1024 * 0.25) - 1 (must be <65536)
  // turn on CTC mode
  TCCR1B |= (1 << WGM12);
  // Set CS12, CS11 and CS10 bits for 1024 prescaler
  TCCR1B |= (1 << CS12) | (0 << CS11) | (1 << CS10);
  // enable timer compare interrupt
  TIMSK1 |= (1 << OCIE1A);
  sei(); // allow interrupts
}

//routine to execute in timer 1 tick
ISR(TIMER1_COMPA_vect) {
  counter++;
  if (counter == 7) { //this is 28 seconds.
    counter = 0;
    if (tempC_1_backup != 150)
      tempC_1_backup = tempC_1;
    if (tempC_2_backup != 150)
      tempC_2_backup = tempC_2;
    if (tempC_3_backup != 150)
      tempC_3_backup = tempC_3;
  }

  //sending data to ESP in this format: "t25aybncy" which means temperature is 25, sensor 1 is correct(ay), 2 is incorrect(bn) and 3 is correct too (cy).
  nano_esp_serial.print("t");
  nano_esp_serial.print(tempValue);
  nano_esp_serial.print("a");
  if (sensor_1_flag)
    nano_esp_serial.print("y");
  else
    nano_esp_serial.print("n");

  nano_esp_serial.print("b");
  if (sensor_2_flag)
    nano_esp_serial.print("y");
  else
    nano_esp_serial.print("n");

  nano_esp_serial.print("c");
  if (sensor_3_flag)
    nano_esp_serial.print("y");
  else
    nano_esp_serial.print("n");

  updateSerial();
}

void handle_temperature_fault(int num) {
  String msg;
  msg = "There is a Problem with Sensor # ";
  msg += num;
  Serial.println(msg);
}

void discoverOneWireDevices() {
  uint8_t sensor_count = 3;
  for (uint8_t j = 0; j < sensor_count; j++) {
    unsigned char addr[8];
    if (j == 0)
      while (oneWire_1.search(addr))
        for (uint8_t i = 0; i < 8; i++)
          Probe_01[i] = addr[i];
    else if (j == 1)
      while (oneWire_2.search(addr))
        for (uint8_t i = 0; i < 8; i++)
          Probe_02[i] = addr[i];
    else if (j == 2)
      while (oneWire_3.search(addr))
        for (uint8_t i = 0; i < 8; i++)
          Probe_03[i] = addr[i];
    if (j == 0)
      oneWire_1.reset_search();
    else if (j == 1)
      oneWire_2.reset_search();
    else if (j == 2)
      oneWire_3.reset_search();
  }

}

//routine to show on LCD
void draw(void) {
  char str[6];
  //write temperatue value
  sprintf(str, "%d", tempValue);
  u8g.setFont(u8g_font_fub42n);
  if(valid_sensors<1){ //all sensors are disconnected
    u8g.setFont(u8g_font_profont29r);
    u8g.drawStr(25, 40, "ERROR");
  }
  
  else if(valid_sensors<3){ //some sensors are disconnected, but not all of them.
    u8g.drawStr(1, 48, str);
  
    //write correct sensors
    u8g.setFont(u8g_font_04b_03b);
      u8g.drawStr(98, 18, "SENSOR");
    if (!sensor_1_flag) {
      u8g.drawStr(98, 26, "#1 ");
      u8g.drawStr(112, 26, "ERR");
    }
    else {
      u8g.drawStr(98, 26, "     ");
    }
    if (!sensor_2_flag) {
      u8g.drawStr(98, 34, "#2 ERR");
    }
    else {
    u8g.drawStr(98, 34, "     ");
    }
    if (!sensor_3_flag) {
      u8g.drawStr(98, 42, "#3 ERR");
    }
    else {
      u8g.drawStr(98, 42, "     ");
    }
  }
  else{ //all sensor are correct
    if(tempValue<-9)
      u8g.drawStr(20, 48, str);
    else if(tempValue>-10 && tempValue<0)
      u8g.drawStr(35, 48, str);
     else if(tempValue>=0 && tempValue<10)
      u8g.drawStr(48, 48, str);
     else
      u8g.drawStr(30, 48, str);
  }

  //write sim card status
  u8g.setFont(u8g_font_04b_03b);
  if (sim_available)
    u8g.drawStr(23, 60, "SIMCARD AVAILABLE");
  else
    u8g.drawStr(15, 60, "SIMCARD NOT AVAILABLE");

  u8g.drawBox(0,                    0,                    max_x_len,  border_len);
  u8g.drawBox(0,                    max_y_len - border_len, max_x_len,  border_len);
  u8g.drawBox(0,                    0,                    border_len, max_y_len);
  u8g.drawBox(max_x_len - border_len, 0,                    max_x_len,  max_y_len);
}


void setup(void) {
  // flip screen, if required
  // u8g.setRot180();
  Serial.begin(nano_serial_baudrate);
  nano_esp_serial.begin(nano_to_esp_baudrate);
  discoverOneWireDevices();

  sensors_1.begin();
  sensors_2.begin();
  sensors_3.begin();
  sensors_1.setResolution(Probe_01, 10);
  sensors_2.setResolution(Probe_02, 10);
  sensors_3.setResolution(Probe_03, 10);
  
  initTimer4s();
  Serial.println("Temperature Started");
}

void loop(void) {
  updateSerial();
  calculate_temperature();
  u8g.firstPage();
  do {
    draw();
  } while ( u8g.nextPage() );
}
void updateSerial() {
  delay(500);
  //receive sim status from ESP. 1 means sim detected and 0 means no sim.
  String simStatus = "";
  while (nano_esp_serial.available()) {
    simStatus += (char)nano_esp_serial.read();
  }
  if (simStatus.length() > 1) {
    if (simStatus.charAt(1) == '1')
      sim_available = true;
    else if (simStatus.charAt(1) == '0')
      sim_available = false;
  }

  while (Serial.available()) {
    nano_esp_serial.write(Serial.read());
  }

}
w