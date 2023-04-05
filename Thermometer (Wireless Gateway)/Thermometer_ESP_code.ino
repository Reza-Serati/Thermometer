/*************************************************************************************************************************************************
 *  "Thermometer ESP8266 Code"
 *  This sketch sets ESP8266 as Access point and Web Server. In the web page you can set username, password and IP address of Access Point.
 *  You can also set a maximum value for temperature and a phone number in the web page. If current temperature exceeds the maximum you set,
 *  ESP8266 will use GSM GA6 Mini module to send a warning sms to the phone number.
 *  Additionally, ESP8266 has a serial communication with Arduino Nano board. It receivess current temperature and correctness or incorrectness of
 *  temperature sensors from Nano. In the other hand, ESP8266 sends simcard status got from GSM to Nano.
 *************************************************************************************************************************************************/

#include <Ticker.h>
#include <EEPROM.h>
#include <eeprm.h>
#include <SoftwareSerial.h>
#include <ESP8266WebServer.h>

SoftwareSerial esp_gsm_serial(D1, D2); //GSM     Tx & Rx   are connected to Esp8266    D1 & D2
SoftwareSerial esp_nano_serial(D4, D3);//Nano     A3 & A1   are connected to Esp8266     D4 & D3

#define esp_to_gsm_buadrate    57600
#define esp_to_nano_buadrate  2400
#define esp_serial_baudrate      115200

//constant addresses in ESP8266 EEPROM
#define phone_number_field  3
#define max_temp_field  4
#define ssid_field 6
#define password_field 7
#define ip_field 9

//default configuration values.
const String defNumber = "+989000000000";
const String defMaxTemp = "30";
const String defSSID = "Thermometer";
const String defPass = "12345678";
const String defIP = "1.1.1.1";

//password of admin, used for reset factory option.
const String adminPass = "thermoadmin1234";

long int maxTemp; //equals to defMaxTemp in the first run.
String maximum_temperature;
long int tempValue = -10;

//these booleans are true if sensorX is working correctly, and false otherwise.
boolean sensor1 = true;
boolean sensor2 = true;
boolean sensor3 = true;
//if a sensor flag is false and 10 min spent, its below boolean becomes true.
boolean sen1_sms_required = false;
boolean sen2_sms_required = false;
boolean sen3_sms_required = false;

Ticker updater; //timer to execute updateSerial()
const uint16_t Counter_Coef = 1200;//change this number for another interval before resending sms.
uint16_t counter = 0;//counts 750 milliseconds until Counter_Coef*750(=15 min), and then resend the temperature warning sms.(not sensor fault sms)

Ticker simTick; //timer for sending simcard status to nano.
const uint8_t failure_coef = 120; //change this number for another waiting time before sending sms for a sensor failure.
uint8_t failure1_counter = 0; //counts 5 seconds until failure_coef*5(=10 min), and then send sensor fault sms.
uint8_t failure2_counter = 0;
uint8_t failure3_counter = 0;

//wifi
ESP8266WebServer server(80);//80 is port number
String ssid; // equals to defSSID in the first run.
String password; //equals to defPass in the first run.
String ip; //equals to deIP in the first run.

//Simcard
boolean serial_flag = false;
String hw_serial_buffer = "";
String my_number; //equals to defNumber in the first run.
String country_code = "+98";//change this code to send sms to another country.
boolean temp_sms_sent = false; //for sending sms one time in temperature exceeded situation.
boolean sensor1_sms_sent = false; //for sending sms one time in impaired sensor situation.
boolean sensor2_sms_sent = false;
boolean sensor3_sms_sent = false;
String temp_exceed = "Warning: Temperature has exceeded from maximum temperature!";
String sensor_fault = "There is a problem with sensor number "; //this string should be concatenated with sensor number.
String sms_prototype = "Massage from A6";



void setup() {
  Serial.begin(esp_serial_baudrate);
  esp_gsm_serial.begin(esp_to_gsm_buadrate);
  esp_nano_serial.begin(esp_to_nano_buadrate);

  Serial.println();
  Serial.println("Initializing...");

  EEPROM.begin(512);
  Eeprm Eeprm(0);
  my_number = Eeprm.readdata(phone_number_field);
  maximum_temperature = Eeprm.readdata(max_temp_field);
  ssid = Eeprm.readdata(ssid_field);
  password = Eeprm.readdata(password_field);
  ip = Eeprm.readdata(ip_field);
  maxTemp = maximum_temperature.toInt();
  const char* username = ssid.c_str();
  const char* pass = password.c_str();
  Serial.println(my_number);
  Serial.println(maxTemp);
  //converting String ip to IPAddress myIP
  uint16_t ip_parts[4] = {0, 0, 0, 0};
  uint8_t indx = 0;
  for (uint8_t i = 0; i < ip.length(); i++) {
    if (ip[i] != '.') {
      ip_parts[indx] *= 10;
      ip_parts[indx] += ip[i] - '0';
    }
    else {
      indx++;
    }
  }
  WiFi.mode(WIFI_AP);
  IPAddress myIP(ip_parts[0], ip_parts[1], ip_parts[2], ip_parts[3]);
  IPAddress gateway(ip_parts[0], ip_parts[1], ip_parts[2], 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(myIP, gateway, subnet);
  WiFi.softAP(username, pass);

  Serial.print("AP IP address: ");
  Serial.println(myIP);
  Serial.println("Temperature Started");

  server.on("/",                  HTTP_GET,   handleRoot);
  server.on("/send_form",  HTTP_POST, handleSendForm);
  server.on("/reset_request",  HTTP_POST, handleResetRequest);
  server.begin();

  send_AT();
  get_signal_quality();

  updater.attach(0.75, updateSerial); //set timer to tick every 750 milliseconds.
  simTick.attach(5, simStatus); //set timer to tick every 5 seconds
}

void loop() {
  server.handleClient();
}

void updateSerial() {
  while (Serial.available()) { //checking hardware serial
    hw_serial_buffer += (char)Serial.read();
    serial_flag = true;
  }

  //check for temperature exceeding
  counter++;
  if (counter == Counter_Coef) { //15 min spent. should resend sms if temperature is more than maximum temperature yet.
    temp_sms_sent = false;
    counter = 0;
  }
  if (tempValue > maxTemp && !temp_sms_sent) {
    temp_sms_sent = true; //to avoid resending sms evey moment.
    counter = 0;
    Serial.print("sending sms to:");
    Eeprm Eeprm(0);
    my_number = Eeprm.readdata(phone_number_field);
    Serial.println(my_number);
    maximum_temperature = Eeprm.readdata(max_temp_field);
    send_sms(my_number, temp_exceed);
  }

  //check for sensor 1 correctness
  if (sen1_sms_required && !sensor1_sms_sent) {
    sensor1_sms_sent = true;
    sen1_sms_required = false;
    Serial.println("sending sms for sensor fault 1");
    Eeprm Eeprm(0);
    my_number = Eeprm.readdata(phone_number_field);
    send_sms(my_number, sensor_fault + "1");
  }
  //check for sensor 2 correctness
  if (sen2_sms_required && !sensor2_sms_sent) {
    sensor2_sms_sent = true;
    sen2_sms_required = false;
    Serial.println("sending sms for sensor fault 2");
    Eeprm Eeprm(0);
    my_number = Eeprm.readdata(phone_number_field);
    send_sms(my_number, sensor_fault + "2");
  }
  //check for sensor 3 correctness
  if (sen3_sms_required && !sensor3_sms_sent) {
    sensor3_sms_sent = true;
    sen3_sms_required = false;
    Serial.println("sending sms for sensor fault 3");
    Eeprm Eeprm(0);
    my_number = Eeprm.readdata(phone_number_field);
    send_sms(my_number, sensor_fault + "3");
  }

  //do the job requested in hardware serial.
  if (serial_flag) {
    serial_flag = false;
    if (hw_serial_buffer == "AT+COPS=?") {
      get_list_of_operators();
      return;
    }
    if (hw_serial_buffer == "AT+COPS?") {
      check_which_network_connected();
      return;
    }
    if (hw_serial_buffer == "send\r\n") {
      Serial.print("sending sms to:");
      Eeprm Eeprm(0);
      my_number = Eeprm.readdata(phone_number_field);
      Serial.println(my_number);
      maximum_temperature = Eeprm.readdata(max_temp_field);
      send_sms(my_number, sms_prototype);
      return;
    }
    if (hw_serial_buffer == "read\r\n") {
      Serial.println("Reading sms");
      read_sms();
      return;
    }
    if (hw_serial_buffer == "ussd\r\n") {
      Serial.println("requesting Ussd");
      send_ussd();
      return;
    }

    for (int i = 0; i < hw_serial_buffer.length(); i++)
      esp_gsm_serial.write(hw_serial_buffer.charAt(i));

    delay(150);
    for (int i = 0; i < hw_serial_buffer.length(); i++)
      esp_nano_serial.write(hw_serial_buffer.charAt(i));

    hw_serial_buffer = "";
  }

  //get response of CPIN commannd and send the result to Nano
  String response = "";
  while (esp_gsm_serial.available()) {
    response += (char)esp_gsm_serial.read();
  }
  uint16_t j = 0;
  while (j + 10 < response.length() && !(response.charAt(j) == '+' && response.charAt(j + 1) == 'C' && response.charAt(j + 2) == 'P' && response.charAt(j + 3) == 'I' && response.charAt(j + 4) == 'N')) {
    j++;
  }
  if (j + 10 < response.length()) {
    //if response is "READY", then simcard is detected.
    if (response.charAt(j + 6) == 'R' && response.charAt(j + 7) == 'E' && response.charAt(j + 8) == 'A' && response.charAt(j + 9) == 'D' && response.charAt(j + 10) == 'Y') {
      esp_nano_serial.print("s");
      esp_nano_serial.print("1");
    }
    //if response is not "READY", then simcard is not detected. it may caused by missing network, not by really simcard absense.
    else {
      esp_nano_serial.print("s");
      esp_nano_serial.print("0");
    }
  }

  //receive data from Nano in this format: "t25aybncy" that means temperature is 25 now, sensor 1 is correct(ay), 2 is incorrect(bn), and 3 is correct too(cy).
  String dataRead = "";
  while (esp_nano_serial.available()) {
    dataRead += (char)(esp_nano_serial.read());
  }
  
  if (dataRead.length() >= 8) {
    uint8_t i = 2;//set to 2, because if dataRead.charAt(1)=='0', then we should go to charAt(2) for being on 'a'
    if (dataRead.charAt(1) != '0') {
      if (dataRead.charAt(1) == '-') //temperature is negative.
        i = 2;
      else
        i = 1;
      uint8_t coef = 1;
      tempValue = 0;
      while (dataRead.charAt(i) != 'a') {
        tempValue = tempValue * coef + dataRead.charAt(i) - 48;
        coef = coef * 10;
        i++;
      }
      if (dataRead.charAt(1) == '-')
        tempValue = tempValue * (-1);
    }
    //now tempValue is set and i is on 'a'
    i++;
    if (dataRead.charAt(i) == 'n') {
      sensor1 = false;
    }
    else{
      sensor1 = true;
      sensor1_sms_sent = false;
    }
    i = i + 2;
    if (dataRead.charAt(i) == 'n') {
      sensor2 = false;
    }
    else{
      sensor2 = true;
      sensor2_sms_sent = false;
    }
    i = i + 2;
    if (dataRead.charAt(i) == 'n') {
      sensor3 = false;
    }
    else{
      sensor3 = true;
      sensor3_sms_sent = false;
    }
  }
  if (temp_sms_sent && tempValue < maxTemp) {
    temp_sms_sent = false;
  }

}

//this routine is called every 5 seconds.
void simStatus() {
  if(!sensor1 && !sensor1_sms_sent){
    failure1_counter++;
    if(failure1_counter==failure_coef){ //10 min spent from first time we discovered sensor 1 failure
      failure1_counter = 0;
      sen1_sms_required = true;
    }
  }
  else if(sensor1 && failure1_counter!=0) //before we arrive at 10 min, sensor1 has been repaired. so, reset the counter
    failure1_counter = 0;
    
  //repeat for sensor 2 and 3
  if(!sensor2 && !sensor2_sms_sent){
    failure2_counter++;
    if(failure2_counter==failure_coef){
      failure2_counter = 0;
      sen2_sms_required = true;
    }
  }
  else if(sensor2 && failure2_counter!=0)
    failure2_counter = 0;
    
  if(!sensor3 && !sensor3_sms_sent){
    failure3_counter++;
    if(failure3_counter==failure_coef){
      failure3_counter = 0;
      sen3_sms_required = true;
    }
  }
  else if(sensor3 && failure3_counter!=0)
    failure3_counter = 0;

  //send CPIN command to GSM.
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("CPIN?");

}

void get_manufacturer_info() {
  esp_gsm_serial.println("ATI"); //check version, manufacturer info
  updateSerial();
}
void check_if_network_registered() {
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("CREG?"); //check if network register
  updateSerial();
}
void send_AT() {
  esp_gsm_serial.println("AT"); //Once the handshake test is successful, it will back to OK
  updateSerial();
}
void get_signal_quality() {
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("CSQ"); //Signal quality test, value range is 0-31 , 31 is the best
  updateSerial();
}
void read_sim_info() {
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("CCID"); //Read SIM information to confirm whether the SIM is plugged
  updateSerial();
}

void check_if_sim_registerd() {
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("CREG?"); //Check whether it has registered in the network
  updateSerial();
}
void get_list_of_operators() {
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("COPS=?");
}
void check_which_network_connected() {
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("COPS?");
}
void send_sms(String telephone_number, String msg) {
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("CMGF=1");

  esp_gsm_serial.print("AT+");
  esp_gsm_serial.print("CMGS=\"");
  esp_gsm_serial.print(telephone_number);
  esp_gsm_serial.println("\"");

  esp_gsm_serial.print(msg);

  esp_gsm_serial.write(26);
}
void set_commiunication_baudrate() {
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.print("IPR=");
  esp_gsm_serial.print("115200");
}
void read_sms() {
  /*
      Its response starts with +CMT: All the fields in the response are comma-separated with
      1. field being phone number.
      2. is the name of person sending SMS.
      3. is a timestamp
      4. is the actual message.
  */
  /*
    If your message is long enough just like ours, then you’ll probably receive it with some
    missing characters. This is not because of a faulty code.
    Your SoftwareSerial receive buffer is getting filled up and discarding characters.
    You are not reading fast enough from the buffer.

    The simplest solution to this is to increase the size of the SoftwareSerial buffer from its
    default size of 64 bytes to 512 bytes (or smaller, depending on what works for you).
  */
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("CMGF=1"); // Configuring TEXT mode
  updateSerial();

  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("CNMI=1,1,0,0,0"); // Decides how newly arrived SMS messages should be handled
  updateSerial();
}
void see_messages() {

  /*
      0 "REC UNREAD" received unread message (i.e. new message)
      1 "REC READ" received read message
      2 "STO UNSENT" stored unsent message (only applicable to SMs)
      3 "STO SENT" stored sent message (only applicable to SMs)
      4 "ALL" all messages (only applicable to +CMGL command)

      AT+CMGL=n  (n=0,1,2,3,4)
  */
}
void send_ussd() {

  String balance_account_req = "*555*1*2#";
  //List of supported responses
  //   esp_gsm_serial.print("AT+");
  //   esp_gsm_serial.println("CUSD=?");
  //   delay(50);

  //  Status of result code presentation
  //  0 – result code presentation is disabled, 1- result code presentation is enabled ,
  //  esp_fsm_serial.print("AT+");
  //   esp_gsm_serial.println("CUSD?");
  //   delay(50);

  //  Enable USSD result code presentation
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.println("CMGF=1");
  delay(50);

  //  Enable USSD result code presentation
  esp_gsm_serial.print("AT+");
  esp_gsm_serial.print("CSCS=");
  esp_gsm_serial.println("\"GSM\"");
  delay(50);

  esp_gsm_serial.print("AT+");
  esp_gsm_serial.print("CUSD=");
  esp_gsm_serial.print("1,\"");
  esp_gsm_serial.print(balance_account_req);
  esp_gsm_serial.println("\",15");
}


void handleRoot() {
  server.send(200, "text/html", prepareHtmlPage());
}

//this function reads value of each field in the web page and set it into suitable variable. it also write values to right places of ESP8266 EEPROM.
void handleSendForm() {
  my_number = server.arg("phoneNumber");
  ssid = server.arg("userName");
  password = server.arg("password");
  maximum_temperature = server.arg("maximumTemperature");
  ip = server.arg("IPaddress");
  
  maxTemp = maximum_temperature.toInt();
  //user enters phone number without country code. we do some process on it to make it suitable for CMGS command.(in send_sms routine)
  my_number.remove(0, 1); //removing first character of number (that is 0)
  my_number = country_code + my_number; //adding country code to first of the number.
  
  Eeprm Eeprm(0);
  Eeprm.writedata(phone_number_field, my_number);
  Eeprm.writedata(max_temp_field, maximum_temperature);
  Eeprm.writedata(ssid_field, ssid);
  Eeprm.writedata(password_field, password);
  Eeprm.writedata(ip_field, ip);
  EEPROM.commit();//This is necessary to call this function to write data in EEPROM. Otherwise, data will stay in RAM and not in EEPROM.

  server.send(200, "text/html", configurationDone());
}

//this function reads admin password input value in reset pop-up and if this is correct, changes configuration to default.
void handleResetRequest(){
  String enteredPass = server.arg("psw");
  if(enteredPass.equals(adminPass)){
    Serial.println("correct admin");

    maximum_temperature = defMaxTemp;
    my_number = defNumber;
    ssid = defSSID;
    password = defPass;
    ip = defIP;
    
    maxTemp = maximum_temperature.toInt();
    
    Eeprm Eeprm(0);
    Eeprm.writedata(phone_number_field, my_number);
    Eeprm.writedata(max_temp_field, maximum_temperature);
    Eeprm.writedata(ssid_field, ssid);
    Eeprm.writedata(password_field, password);
    Eeprm.writedata(ip_field, ip);
    EEPROM.commit();//This is necessary to call this function to write data in EEPROM. Otherwise, data will stay in RAM and not in EEPROM.
  
    server.send(200, "text/html", resetDone());
  }
  else{
    server.send(200, "text/html", wrongPass());
  }
}

//HTML code of configurationDone page. This page will appear after pressing confirm button in configuration page.
String configurationDone() {
  String webPage;
  webPage += "<!DOCTYPE html>";
  webPage += "<html lang=\"en\">";
  webPage += "<head>";
  webPage += "<title>Config Done</title> <meta charset=\"UTF-8\">";
  webPage += "<style>";
  webPage += "button { outline: none !important; border: none; background: transparent; } button:hover { cursor: pointer; } .limiter { width: 100%; margin: 0 auto; } .container-login100 { width: 100%; min-height: 100vh; display: -webkit-box; display: -webkit-flex; display: -moz-box; display: -ms-flexbox; ";
  webPage += "display: flex; flex-wrap: wrap; justify-content: center; align-items: center; padding: 15px; background: #87B; } .wrap-login100 { width: 390px; background: #fff; border-radius: 10px; ";
  webPage += "overflow: hidden; padding: 77px 55px 33px 55px; box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -moz-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -webkit-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); ";
  webPage += "-o-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -ms-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); } .container-login100-form-btn { display: -webkit-box; display: -webkit-flex; ";
  webPage += "display: -moz-box; display: -ms-flexbox; display: flex; flex-wrap: wrap; justify-content: center; padding-top: 13px; } .wrap-login100-form-btn { width: 100%; ";
  webPage += "display: block; position: relative; z-index: 1; border-radius: 25px; overflow: hidden; margin: 0 auto; } .login100-form-bgbtn { position: absolute; z-index: -1; width: 300%; ";
  webPage += "height: 100%; background: #a64bf4; background: -webkit-linear-gradient(right, #21d4fd, #b721ff, #21d4fd, #b721ff); background: -o-linear-gradient(right, #21d4fd, #b721ff, #21d4fd, #b721ff); ";
  webPage += "background: -moz-linear-gradient(right, #21d4fd, #b721ff, #21d4fd, #b721ff); background: linear-gradient(right, #21d4fd, #b721ff, #21d4fd, #b721ff); top: 0; left: ";
  webPage += "-100%; -webkit-transition: all 0.4s; -o-transition: all 0.4s; -moz-transition: all 0.4s; transition: all 0.4s; } .login100-form-btn { font-size: 25px; color: #fff; line-height: 1.5; ";
  webPage += "padding: 0 25px; width: 100%; height: 60px; } .wrap-login100-form-btn:hover .login100-form-bgbtn { left: 0; } @media (max-width: 576px) { .wrap-login100 { padding: 77px 15px 33px 15px; } }";
  webPage += "</style>";
  webPage += "</head>";
  webPage += "<body>";
  webPage += "<div class=\"limiter\"> <div class=\"container-login100\"> <div class=\"wrap-login100\">";
  webPage += "<h1><center>تغييرات با موفقيت اعمال شد<center></h1><br>";
  webPage += "<center><div style='font-size:100px;'>&#9989;</div><center>";
  webPage += "<div class=\"container-login100-form-btn\"> <div class=\"wrap-login100-form-btn\"> <div class=\"login100-form-bgbtn\"></div> <button class=\"login100-form-btn\" onclick=\"goBack()\">";
  webPage += "بازگشت";
  webPage +=  "</button> </div> </div> </div> </div> </div>";
  webPage += "<script>function goBack() {window.history.back();}</script>";
  webPage += "</body></html>";

  return webPage;
}

//HTML code of reset done page. it will be shown after confirming admin password for reseting configuration.
String resetDone(){
  String webPage = "";
   webPage += "<!DOCTYPE html>";
  webPage += "<html lang=\"en\">";
  webPage += "<head>";
  webPage += "<title>Reset Done</title> <meta charset=\"UTF-8\">";
  webPage += "<style>";
  webPage += ".limiter { width: 100%; margin: 0 auto; } .container-login100 { width: 100%; min-height: 100vh; display: -webkit-box; display: -webkit-flex; display: -moz-box; display: -ms-flexbox; display: flex; flex-wrap: wrap; justify-content: center; align-items: center; padding: 15px; background: #87B; } .wrap-login100 { width: 390px; background: #fff; border-radius: 10px; overflow: hidden; padding: 77px 55px 33px 55px; box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -moz-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -webkit-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -o-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -ms-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); } @media (max-width: 576px) { .wrap-login100 { padding: 77px 15px 33px 15px; } }";
  webPage += "</style>";
  webPage += "</head>";
  webPage += "<body>";
  webPage += "<div class=\"limiter\"> <div class=\"container-login100\"> <div class=\"wrap-login100\">";
  webPage += "<h1><center>بازگشت به تنظیمات اولیه با موفقیت انجام شد<center></h1><br>";
  webPage += "<center><div style='font-size:100px;'>&#9989;</div><center>";
  webPage +=  "</div> </div> </div>";
  webPage += "</body></html>";
  
  return webPage;
}

//HTML code of wrong password page. it will be shown if the password entered by user (on reset pop-up) does not equal to admin password.
String wrongPass(){
  String webPage = "";
   webPage += "<!DOCTYPE html>";
  webPage += "<html lang=\"en\">";
  webPage += "<head>";
  webPage += "<title>Wrong Password</title> <meta charset=\"UTF-8\">";
  webPage += "<style>";
  webPage += ".limiter { width: 100%; margin: 0 auto; } .container-login100 { width: 100%; min-height: 100vh; display: -webkit-box; display: -webkit-flex; display: -moz-box; display: -ms-flexbox; display: flex; flex-wrap: wrap; justify-content: center; align-items: center; padding: 15px; background: #87B; } .wrap-login100 { width: 390px; background: #fff; border-radius: 10px; overflow: hidden; padding: 77px 55px 33px 55px; box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -moz-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -webkit-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -o-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -ms-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); } @media (max-width: 576px) { .wrap-login100 { padding: 77px 15px 33px 15px; } }";
  webPage += "</style>";
  webPage += "</head>";
  webPage += "<body>";
  webPage += "<div class=\"limiter\"> <div class=\"container-login100\"> <div class=\"wrap-login100\">";
  webPage += "<h1><center>رمز عبور وارد شده اشتباه است<center></h1><br>";
  webPage += "<center><div style='font-size:100px;'>&#10060;</div><center>";
  webPage +=  "</div> </div> </div>";
  webPage += "</body></html>";

  return webPage;
}

//HTML code of main web page. after entering ip address of Access point in browser, this page will be shown.
//You can enter your desired values for maximum temperature, phone number, and username, password and IP address of wifi.
//It also povide a button to reset factory option.
String prepareHtmlPage() {
  String webPage;
  webPage += "<!DOCTYPE html>";
  webPage += "<html lang=\"en\">";
  webPage += "<head>";
  webPage += "<title>Config Page</title> <meta charset=\"UTF-8\">";
  webPage += "<style>";
  webPage += "* { background-color: 555; margin: 0px; padding: 0px; box-sizing: border-box; text-align: center; } body, html { height: 100%; font-family: Poppins-Regular, sans-serif; } ";
  webPage += "h1,h2,h3,h4,h5,h6 { margin: 0px; } p { font-family: Poppins-Regular; font-size: 14px; line-height: 1.7; color: #666666; margin: 0px; } ul, li { margin: 0px; list-style-type: none; } ";
  webPage += "input { outline: none; border: none; } textarea { outline: none; border: none; } textarea:focus, input:focus { border-color: transparent !important; } input:focus::-webkit-input-placeholder { color:transparent; } ";
  webPage += "input:focus:-moz-placeholder { color:transparent; } input:focus::-moz-placeholder { color:transparent; } input:focus:-ms-input-placeholder { color:transparent; } ";
  webPage += "textarea:focus::-webkit-input-placeholder { color:transparent; } textarea:focus:-moz-placeholder { color:transparent; } textarea:focus::-moz-placeholder { color:transparent; } ";
  webPage += "textarea:focus:-ms-input-placeholder { color:transparent; } input::-webkit-input-placeholder { color: #adadad;} input:-moz-placeholder { color: #adadad;} ";
  webPage += "input::-moz-placeholder { color: #adadad;} input:-ms-input-placeholder { color: #adadad;} textarea::-webkit-input-placeholder { color: #adadad;} textarea:-moz-placeholder { color: #adadad;} ";
  webPage += "textarea::-moz-placeholder { color: #adadad;} textarea:-ms-input-placeholder { color: #adadad;} button { outline: none !important; border: none; background: transparent; } ";
  webPage += "button:hover { cursor: pointer; } iframe { border: none !important; } .txt1 { font-family: Poppins-Regular; font-size: 13px; color: #666666; line-height: 1.5; } ";
  webPage += ".txt2 { font-family: Poppins-Regular; font-size: 13px; color: #333333; line-height: 1.5; } .limiter { width: 100%; margin: 0 auto; } .reset {width: 100%; height: 10% align-items: left; background: #87B; padding: 30px 50px 50px 50px;}";
  webPage += ".container-login100 { width: 100%; min-height: 100vh; display: -webkit-box; display: -webkit-flex; display: -moz-box; display: -ms-flexbox; display: flex; flex-wrap: wrap; ";
  webPage += "justify-content: center; align-items: center; padding: 15px; background: #87B; } .wrap-login100 { width: 390px; background: #fff; border-radius: 10px; overflow: hidden; ";
  webPage += "padding: 77px 55px 33px 55px; box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -moz-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -webkit-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); ";
  webPage += "-o-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); -ms-box-shadow: 0 5px 10px 0px rgba(0, 0, 0, 0.1); } .login100-form { width: 100%; } ";
  webPage += ".login100-form-title { display: block; font-size: 45px; color: #333333; line-height: 1.2; text-align: center; padding: 25px 0px 10px 0px; } .login100-form-title i { font-size: 60px; } ";
  webPage += ".wrap-input100 { width: 100%; position: relative; border-bottom: 2px solid #adadad; margin-bottom: 37px; } .wrap-input50 { width: 50%; position: relative; border-bottom: 2px solid #adadad; margin: 0 auto; } ";
  webPage += ".input100 { font-family: Poppins-Regular; font-size: 21px; color: #555555; line-height: 1.2; display: block; width: 100%; height: 45px; background: transparent; padding: 0 5px; } ";
  webPage += ".input50 { font-family: Poppins-Regular; font-size: 21px; color: #555555; width: 100%; height: 40px; padding: 0 5px; } .btn-show-pass { font-size: 15px; color: #999999; ";
  webPage += "display: -webkit-box; display: -webkit-flex; display: -moz-box; display: -ms-flexbox; display: flex; align-items: center; position: absolute; height: 100%; top: 0; ";
  webPage += "right: 0; padding-right: 5px; cursor: pointer; -webkit-transition: all 0.4s; -o-transition: all 0.4s; -moz-transition: all 0.4s; transition: all 0.4s; } ";
  webPage += ".btn-show-pass:hover { color: #6a7dfe; color: -webkit-linear-gradient(left, #21d4fd, #b721ff); color: -o-linear-gradient(left, #21d4fd, #b721ff); color: -moz-linear-gradient(left, #21d4fd, #b721ff); ";
  webPage += "color: linear-gradient(left, #21d4fd, #b721ff); } .btn-show-pass.active { color: #6a7dfe; color: -webkit-linear-gradient(left, #21d4fd, #b721ff); color: -o-linear-gradient(left, #21d4fd, #b721ff); ";
  webPage += "color: -moz-linear-gradient(left, #21d4fd, #b721ff); color: linear-gradient(left, #21d4fd, #b721ff); } .container-login100-form-btn { display: -webkit-box; display: -webkit-flex; ";
  webPage += "display: -moz-box; display: -ms-flexbox; display: flex; flex-wrap: wrap; justify-content: center; padding-top: 13px; } .wrap-login100-form-btn { width: 100%; ";
  webPage += "display: block; position: relative; z-index: 1; border-radius: 25px; overflow: hidden; margin: 0 auto; } .login100-form-bgbtn { position: absolute; z-index: -1; width: 300%; ";
  webPage += "height: 100%; background: #a64bf4; background: -webkit-linear-gradient(right, #21d4fd, #b721ff, #21d4fd, #b721ff); background: -o-linear-gradient(right, #21d4fd, #b721ff, #21d4fd, #b721ff); ";
  webPage += "background: -moz-linear-gradient(right, #21d4fd, #b721ff, #21d4fd, #b721ff); background: linear-gradient(right, #21d4fd, #b721ff, #21d4fd, #b721ff); top: 0; left: ";
  webPage += "-100%; -webkit-transition: all 0.4s; -o-transition: all 0.4s; -moz-transition: all 0.4s; transition: all 0.4s; } .login100-form-btn { font-size: 25px; color: #fff; line-height: 1.5; ";
  webPage += "text-transform: uppercase; display: -webkit-box; display: -webkit-flex; display: -moz-box; display: -ms-flexbox; display: flex; justify-content: center; align-items: center; ";
  webPage += "padding: 0 25px; width: 100%; height: 60px; } .wrap-login100-form-btn:hover .login100-form-bgbtn { left: 0; } @media (max-width: 576px) { .wrap-login100 { padding: 77px 15px 33px 15px; } }";
  //these styles are used for reset factory option.
  /* Button used to open the contact form - fixed at the bottom of the page */
  webPage += ".open-button {background-color: #555; color: white; font-size: 18px; padding: 16px 20px; border: none; cursor: pointer; opacity: 0.8; position: fixed; bottom: 23px; right: 28px; width: 280px;}";
  /* The popup form - hidden by default */
  webPage += ".form-popup { display: none; position: fixed; bottom: 0; right: 15px; border: 3px solid #f1f1f1; z-index: 9;}";
  /* Add styles to the form container */
  webPage += ".form-container { max-width: 300px; padding: 10px; background-color: white;}";
  /* Full-width input fields */
  webPage += ".form-container input[type=text], .form-container input[type=password] { width: 100%; padding: 15px; margin: 5px 0 22px 0; border: none; background: #f1f1f1;}";
  /* When the inputs get focus, do something */
  webPage += ".form-container input[type=text]:focus, .form-container input[type=password]:focus { background-color: #ddd; outline: none;}";
  /* Set a style for the submit button */
  webPage += ".form-container .btn { background-color: #4CAF50; color: white; font-size: 14px; padding: 16px 20px; border: none; cursor: pointer; width: 100%; margin-bottom:10px; opacity: 0.8;}";
  /* Add a red background color to the cancel button */
  webPage += ".form-container .cancel { background-color: red; font-size: 14px;}";
  /* Add some hover effects to buttons */
  webPage += ".form-container .btn:hover, .open-button:hover { opacity: 1;}";
  webPage += "</style>";
  webPage += "</head>";
  webPage += "<body>";
  webPage += "<div class=\"limiter\"> <div class=\"container-login100\"> ";
  webPage += "<div class=\"wrap-login100\"> <form class=\"login100-form validate-form\" method=\"POST\" action=\"/send_form\"> <h1 class=\"login100-form-title p-b-26\">";
  webPage += "خوش آمديد";
  webPage += "</h1>";
  webPage += "<span style='font-size:100px;'>&#127777;</span>";
  String number;
  number = my_number;
  number.remove(0, 3);
  number = "0" + number;
  webPage += "<div class=\"wrap-input100\"> <h3>شماره خود را وارد کنيد</h3> <input class=\"input100\" type=\"text\" name=\"phoneNumber\" value=" + number + "></div>";
  webPage += "<div class=\"wrap-input100\"> <h3>حداکثر دما را وارد کنيد</h3> <input class=\"input100\" type=\"text\" name=\"maximumTemperature\" value=" + maximum_temperature + "></div><br>";
  webPage += "<div class=\"wrap-input100\"> <h3>نام کاربري مورد نظر را وارد کنيد</h3> <input class=\"input100\" type=\"text\" name=\"userName\" value=" + ssid + "></div>";
  webPage += "<div class=\"wrap-input100\"> <h3>رمز عبور مورد نظر را وارد کنيد</h3> <input class=\"input100\" type=\"text\" name=\"password\" value=" + password + "></div>";
  webPage += "<div class=\"wrap-input100\"> <h3>آدرس  مورد نظر را وارد کنيد</h3> <input class=\"input100\" type=\"text\" name=\"IPaddress\" value=" + ip + "></div>";
  webPage += "<div class=\"container-login100-form-btn\"> <div class=\"wrap-login100-form-btn\"> <div class=\"login100-form-bgbtn\"></div> <button class=\"login100-form-btn\">";
  webPage += "ذخيره تغييرات";
  webPage += "</button>";
  webPage += "</div> </div> </form> </div> </div> </div>";
  //reset option
  webPage += "<button class=\"open-button\" onclick=\"openForm()\">بازگشت به تنظیمات اولیه</button> <div class=\"form-popup\" id=\"myForm\"> <form method=\"POST\" action=\"/reset_request\" class=\"form-container\">";
  webPage += " <h2 style='padding-bottom: 30px;'>بازگشت به تنظیمات اولیه</h2><label for=\"psw\"><b>رمز عبور ادمین را وارد نمایید</b></label><input type=\"password\" name=\"psw\" required>";
  webPage += " <button type=\"submit\" class=\"btn\">تایید</button>";
  webPage += "<button type=\"button\" class=\"btn cancel\" onclick=\"closeForm()\">بستن</button> </form> </div>";
  webPage += "<script> function openForm() { document.getElementById(\"myForm\").style.display = \"block\";}";
  webPage += "function closeForm() { document.getElementById(\"myForm\").style.display = \"none\";} </script>";
  webPage += "</body></html>";
  
  return webPage;
}
