from pathlib import Path
from reportlab.pdfgen import canvas
from reportlab.lib.pagesizes import A4
from reportlab.lib.colors import HexColor,white
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from textwrap import wrap
R=Path(__file__).resolve().parents[1];P=R/'docs/FireGuard-build-and-test-guide.pdf'
pdfmetrics.registerFont(TTFont('ArialFG','/System/Library/Fonts/Supplemental/Arial.ttf'))
pdfmetrics.registerFont(TTFont('ArialFGB','/System/Library/Fonts/Supplemental/Arial Bold.ttf'))
W,H=A4;c=canvas.Canvas(str(P),pagesize=A4);c.setTitle('FireGuard revision 3 - Build, components and testing')
ink=HexColor('#18332e');muted=HexColor('#53645f');green=HexColor('#eaf2ed');line=HexColor('#d7e0db')
page=0;y=0
def text(x,yy,s,size=10,b=False,color=ink):
 c.setFillColor(color);c.setFont('ArialFGB' if b else 'ArialFG',size);c.drawString(x,yy,s)
def start(title,sub):
 global page,y
 page+=1;c.setFillColor(ink);c.rect(0,H-105,W,105,fill=1,stroke=0)
 text(38,H-34,'FIREGUARD / TWO-SENSOR RELEASE',11,True,white)
 text(38,H-65,title,22,True,white);text(38,H-86,sub,10,False,white);y=H-130
 text(38,25,'Revision 3 | MQ-2 + MQ-135 + ADS1115 + BME280 + camera/CNN',8,False,muted)
 text(W-85,25,f'{page} / 4',8,False,muted)
def heading(s):
 global y
 text(38,y,s,12,True);y-=22
def para(s):
 global y
 words=s.split();rows=[];current=''
 for word in words:
  trial=(current+' '+word).strip()
  if pdfmetrics.stringWidth(trial,'ArialFG',10)>W-76:rows.append(current);current=word
  else:current=trial
 if current:rows.append(current)
 for row in rows:text(38,y,row);y-=14
 y-=8
 assert y>43,(page,s,y)
def table(rows,widths):
 global y
 for i,row in enumerate(rows):
  height=23
  c.setFillColor(green if i==0 else (HexColor('#f6f8f6') if i%2 else white));c.rect(38,y-height,W-76,height,fill=1,stroke=0)
  x=46
  for val,w in zip(row,widths):text(x,y-15,val,9,i==0);x+=w
  y-=height
 y-=17
def end():c.showPage()
start('Components and quantities','Circuit revision L; hybrid location uses built-in Wi-Fi. No GPS module.')
table([
 ['Component / specification','Qty'],
 ['ESP32-S3 N16R8 CAM with attached camera','1'],
 ['MQ-2 module / MQ-135 module','1 each'],
 ['ADS1115 HW-198 / six-pin 3.3 V BME280','1 each'],
 ['Apple 20 W USB-C charger / USB-C to USB-C cable','1 each'],
 ['HW-189 USB-C breakout','1'],
 ['Two-pin 5 V DC active buzzer, <=20 mA','1'],
 ['PN2222A / 1N4007 (inductive-buzzer clamp)','1 each'],
 ['10 kohm 1% resistors / 5.1 kohm resistors','4 / 2'],
 ['1 kohm resistor / 100 kohm resistor','1 / 1'],
 ['100 nF ceramic capacitors','3'],
 ['100 uF electrolytic capacitor, >=10 V','1'],
 ['Normally-open push buttons','3'],
 ['10-pin ADS header / 6-pin BME header','1 each'],
 ['Removable ESP32 5 V feed jumper','1'],
 ['Perfboard or suitable insulated mounting / wire set','1 each'],
 ],[445,50])
heading('Use your existing tested filters')
para('Retain two 10k/10k + 100nF filters. MQ-2 goes to ADS A0; MQ-135 stays on A2. The third prepared filter is spare. MQ DO pins, ADS A1/A3/ALRT and GPIO40 stay unconnected.')
heading('Remove completely')
para('MQ-7, its filter branch, LM2596, heater relay, second transistor and its control resistors. No dedicated CO measurement remains. No OLED, SD card or USB-A adapter is required.')
para('The replacement buzzer specification is required; the photographed original buzzer has no confirmed voltage/current/type rating. This list does not confirm a new buzzer has been purchased.')
end()
start('Wire and power','Use the separate one-page schematic for all connections and component symbols.')
heading('Power path')
para('Charger -> USB-C cable -> HW-189 VCC/GND -> main 5 V/GND rails. CC1 and CC2 each need their OWN 5.1k resistor to GND. Keep CC1 and CC2 separate. Use the standard 5 V output; no 9 V PD trigger.')
para('Main 5 V powers MQ-2 VCC, MQ-135 VCC and buzzer positive. A removable link feeds the ESP32 header marked 5V. ESP32 3V3 feeds ADS VDD and BME VCC. Every ground joins. Put 100uF plus 100nF across the main rail with correct electrolytic polarity.')
table([
 ['Signal','Connection'],['SDA / SCL','GPIO41 / GPIO42, shared ADS + BME'],
 ['ADS ADDR / supply','GND (0x48) / 3V3'],['BME CSB / SDO','3V3 / GND (0x76)'],
 ['MQ-2 filtered AO / MQ-135 filtered AO','ADS A0 / ADS A2'],
 ['B1 / B2 / B3','GPIO14 / GPIO47 / GPIO21 to GND'],
 ['Buzzer control','GPIO38 -> 1k -> PN2222A base'],
 ['Driver off / return','Base -> 100k -> GND; emitter -> GND'],
 ['Driver load','Collector -> buzzer negative'],
 ['Inductive-buzzer diode','Band to +5V; other end to collector'],
 ],[245,245])
heading('Filter detail - repeat once per MQ module')
# Mini wiring figure with labels and junctions.
y0=y-26;c.setStrokeColor(ink);c.setLineWidth(1.2)
c.line(60,y0,170,y0);c.rect(170,y0-8,55,16);c.line(225,y0,430,y0)
c.circle(295,y0,3,fill=1);c.line(295,y0,295,y0-20);c.rect(287,y0-55,16,35);c.line(295,y0-55,295,y0-85)
c.line(365,y0,365,y0-35);c.line(350,y0-35,380,y0-35);c.line(350,y0-41,380,y0-41);c.line(365,y0-41,365,y0-85);c.line(295,y0-85,365,y0-85)
text(60,y0+15,'MQ AO',9,True);text(173,y0+15,'10k',9);text(292,y0+15,'To ADS A0 or A2',9,True);text(310,y0-43,'10k',9);text(382,y0-41,'100nF',9);text(314,y0-100,'GND',9)
y=y0-123
para('Raw AO goes into the FIRST resistor. The middle junction is the filtered output. At 5.00 V input expect about 2.50 V output; generally Vout/Vin is about 0.50. Never connect raw 5 V AO directly to the 3.3 V ADS.')
para('Before PC USB: unplug the charger AND disconnect the external ESP32 5V header feed. The schematic B/C/E labels are electrical terminals; match your actual transistor lead order.')
end()
start('Flash and configure','Matching prebuilt binaries are in release/. No ESP32 was flashed by the agent.')
heading('1. Flash over USB')
para('Disconnect external power first. In the release directory run the command below with esptool installed. Replace YOUR_PORT with the board port. The build targets 16 MB flash and 8 MB octal PSRAM.')
text(38,y,'python -m esptool --chip esp32s3 -p YOUR_PORT -b 460800 write-flash @flash_args',9,True);y-=27
para('Use 115200 baud if flashing is unreliable. Do not erase flash during a normal upgrade. The old R0 and adaptation values are ignored; resident/Wi-Fi/Telegram settings can be retained.')
heading('2. Local setup')
para('Open the native USB serial console at 115200. Read the setup Wi-Fi password. Hold B2 for 5 seconds, join FireGuard-Setup, open http://192.168.4.1. Configure Wi-Fi, Telegram bot, residents and address fields.')
para('Residents join Telegram with /join PIN. Step 05 takes the existing AWS deployment URL plus the PRIVATE device connection key. The dashboard access key is different. These secrets are not embedded in this source ZIP.')
para('Hybrid location: in step 02 save and verify the building coordinates. Optional Google Wi-Fi geolocation needs your API key locally; saved coordinates stay primary. Wi-Fi-only fixes expire after 48 h and must have a reported radius <=2 km. Estimates are not guaranteed GPS accuracy. No extra wiring is required.')
heading('3. Power and readings')
para('After disconnecting PC USB, use your tested USB-C power source. Expected supplies: main rail about 5 V; ADS/BME about 3.3 V. Your reported bench values were 5.11 V and 3.29 V respectively. Check the completed joints and reversed cable orientation before relying on the assembly.')
para('Boot in known clean air. Warm-up lasts 5 minutes; new MQ elements require longer conditioning. Device health should identify two-mq-ads1115-v2, ADS connected, both MQ inputs valid and plausible BME readings. An MQ has no universal clean-air AO voltage.')
heading('4. Buttons and camera')
para('B1 tap mutes the buzzer up to 120 seconds; hold 1.5 seconds advances labels. B2 tap sends Telegram readings; hold 5 seconds opens setup. B3 tap saves an available fresh scored frame; hold 1.5 seconds sends a Telegram test.')
para('Camera/CNN wakes on sensor or thermal demand. The dashboard photo button retrieves the latest uploaded image; it does not trigger capture. Old or unavailable imagery must not be treated as fresh evidence.')
heading('5. Current model')
para('The two MQ channels provide broad relative activity. No ppm, gas identity or CO concentration is claimed. The retained CNN and revised heuristic need validation on your assembled board. Start with test recipients and keep certified alarms in service.')
end()
start('Verify the complete system','Component checks passed earlier; final wiring and delivery still require verification.')
heading('A. Electrical and ADC')
para('[ ] Power OFF: inspect bridges, polarity, transistor terminals and ground connections. With lab/meter access, recheck both supply rails and divider ratios after final soldering. Do not infer a new joint is sound from an earlier clip test.')
para('[ ] ADC scaling: four conversions per channel are averaged. ADS 1 count = 0.125 mV at the configured gain. Dashboard AO mV = 2 x ADC mV. A divider fed with 3.30 V should give about 1.65 V (1650 mV ADC; 3300 mV displayed AO). Disconnect the sensor AO before applying a known test input; never connect two outputs together.')
para('[ ] Fault test: power OFF, disconnect an input/module, reboot and inspect input validity and fault flags after warm-up. Restore power OFF. A floating input can read a plausible voltage, so firmware cannot prove physical continuity from voltage alone.')
heading('B. Firmware, camera and dashboard')
para('[ ] Confirm warm-up, BME readings, buzzer, all buttons, real Telegram readings, camera demand/fresh image and camera-fault notice. Check sample cadence, power stability, heap and task stacks during CNN plus HTTPS operation.')
para('[ ] AWS: authenticate at the existing URL. After setup sync, expect the real hardware status to replace Waiting for ESP32. Power the board down: after about 25 seconds the dashboard should label last known status offline. Never treat synthetic test readings as hardware evidence.')
heading('C. Telegram and optional SMS')
para('[ ] Test resident join, readings, explicit test alert and event acknowledgement. Emergency forwarding starts OFF. A test emergency recipient must join with /emergency PIN. Initial forwarding tests must use test mode; the default unattended warning interval is 60 seconds.')
para('[ ] Optional station SMS needs textbee, a full station/test number, the separate station-SMS switch and master forwarding. Use your own phone and TEST mode first. SMS contains an address excerpt, map, postal code and callback; it does not attach the photo. See LOCATION_SETUP.md.')
para('[ ] textbee starts OFF: Android phone + working SIM + internet are required. Enter its API key locally in step 06. AWS SNS starts OFF: verify your test destination while in the regional sandbox, satisfy route requirements, then explicitly redeploy with SMS enabled. Follow the two SMS setup guides in docs/.')
para('[ ] Confirm real handset receipt; API acceptance is not delivery. Both resident SMS providers enabled can duplicate alerts. Do not test with an emergency-service number.')
heading('D. Performance evidence')
para('[ ] Collect labelled clean-air, cooking/steam and safe representative scenes. Measure nuisance alarms and misses before changing thresholds. Do not deliberately release gas, generate CO or expose the prototype to a real fire. No CO channel or certified detection performance exists in this design.')
end();c.save();print(P)
