"""Current two-gas FireGuard schematic; identical PNG and vector PDF geometry."""
from pathlib import Path
import math
import shutil
from PIL import Image, ImageDraw, ImageFont
from reportlab.pdfgen import canvas
from reportlab.lib.pagesizes import A3, landscape
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from pypdf import PdfReader

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'docs'; WORK=ROOT/'work/circuit'; WORK.mkdir(parents=True,exist_ok=True)
W,H=4800,3394
im=Image.new('RGB',(W,H),'white');d=ImageDraw.Draw(im)
REG='/System/Library/Fonts/Supplemental/Arial.ttf'
BOLD='/System/Library/Fonts/Supplemental/Arial Bold.ttf'
pdfmetrics.registerFont(TTFont('FGCircuit',REG));pdfmetrics.registerFont(TTFont('FGCircuitBold',BOLD))
pdfout=OUT/'FireGuard-updated-circuit.pdf'
pw,ph=landscape(A3)
c=canvas.Canvas(str(pdfout),pagesize=(pw,ph))
c.setTitle('FireGuard - Updated Circuit - MQ-2, MQ-135, BME280 and Camera - Rev L')
c.setAuthor('FireGuard')
scale=min((pw-24)/W,(ph-24)/H)
c.translate((pw-W*scale)/2,(ph-H*scale)/2);c.scale(scale,scale)

def t(x,y,s,n=29,b=False,anchor=None):
    font=ImageFont.truetype(BOLD if b else REG,n)
    # Text uses a true top anchor for matching engineering layout in both outputs.
    a='lt' if anchor is None else {'rt':'rt','mt':'mt'}.get(anchor,anchor)
    d.text((x,y),s,font=font,fill='black',anchor=a)
    name='FGCircuitBold' if b else 'FGCircuit';c.setFont(name,n);c.setFillColorRGB(0,0,0)
    baseline=H-y-n*.73
    if a.startswith('r'):c.drawRightString(x,baseline,s)
    elif a.startswith('m'):c.drawCentredString(x,baseline,s)
    else:c.drawString(x,baseline,s)

def ln(points,w=4):
    d.line(points,fill='black',width=w,joint='curve')
    c.setLineWidth(w);c.setStrokeColorRGB(0,0,0)
    p=c.beginPath();p.moveTo(points[0][0],H-points[0][1])
    for x,y in points[1:]:p.lineTo(x,H-y)
    c.drawPath(p)

def box(x,y,w,h,width=4):
    d.rectangle((x,y,x+w,y+h),outline='black',width=width)
    c.setLineWidth(width);c.rect(x,H-y-h,w,h,stroke=1,fill=0)

def ellipse(x1,y1,x2,y2,fill=False,width=4):
    d.ellipse((x1,y1,x2,y2),outline='black',fill='black' if fill else None,width=width)
    c.setLineWidth(width);c.ellipse(x1,H-y2,x2,H-y1,stroke=1,fill=int(fill))

def dot(x,y):ellipse(x-7,y-7,x+7,y+7,True,1)
def ground(x,y):
    ln([(x,y),(x,y+18)])
    for off,r in [(18,27),(31,18),(44,8)]:ln([(x-r,y+off),(x+r,y+off)])
def power(x,y,label):
    ln([(x,y),(x,y-24),(x-14,y-9)]);ln([(x,y-24),(x+14,y-9)])
    t(x,y-66,label,28,True,'mt')
def label(x,y,s,side='right',n=27):
    tw=d.textlength(s,font=ImageFont.truetype(BOLD,n))
    if side=='right':ln([(x,y),(x+20,y)]);xx=x+20
    else:ln([(x-20,y),(x,y)]);xx=x-20-tw-24
    box(xx,y-24,tw+24,48,2);t(xx+12,y-17,s,n,True)
def nc(x,y):
    ln([(x-8,y-8),(x+8,y+8)],3);ln([(x-8,y+8),(x+8,y-8)],3)
def rh(x1,x2,y,ref,val):
    m=(x1+x2)/2;ln([(x1,y),(m-43,y)]);box(m-43,y-17,86,34);ln([(m+43,y),(x2,y)])
    t(m-55,y-87,ref,27);t(m-55,y-52,val,28)
def rv(x,y1,y2,ref,val,side=1):
    m=(y1+y2)/2;ln([(x,y1),(x,m-41)]);box(x-16,m-41,32,82);ln([(x,m+41),(x,y2)])
    xx=x+30 if side==1 else x-175;t(xx,m-37,ref,27);t(xx,m,val,28)
def cap(x,y1,y2,ref,val,pol=False):
    m=(y1+y2)/2;ln([(x,y1),(x,m-10)]);ln([(x-31,m-10),(x+31,m-10)])
    ln([(x-31,m+10),(x+31,m+10)],8 if pol else 4);ln([(x,m+10),(x,y2)])
    t(x+46,m-35,ref,26);t(x+46,m+3,val,27)
    if pol:t(x-57,m-55,'+',28)
def notes(x,y,lines,n=27,step=43):
    for i,s in enumerate(lines):t(x,y+i*step,s,n)
def hop(x1,x2,y,cross):
    ln([(x1,y),(cross-19,y)])
    d.arc((cross-19,y-19,cross+19,y+19),180,360,fill='black',width=4)
    c.setLineWidth(4);c.arc(cross-19,H-y-19,cross+19,H-y+19,startAng=0,extent=180)
    ln([(cross+19,y),(x2,y)])
def polygon(pts,fill=False,width=4):
    d.polygon(pts,fill='black' if fill else None,outline='black',width=width)
    p=c.beginPath();p.moveTo(pts[0][0],H-pts[0][1])
    for x,y in pts[1:]:p.lineTo(x,H-y)
    p.close();c.setLineWidth(width);c.drawPath(p,stroke=1,fill=int(fill))
def arrow(a,b):
    ln([a,b]);ang=math.atan2(b[1]-a[1],b[0]-a[0]);le=24
    polygon([b,(b[0]-le*math.cos(ang-.4),b[1]-le*math.sin(ang-.4)),
             (b[0]-le*math.cos(ang+.4),b[1]-le*math.sin(ang+.4))],True)

box(45,45,W-90,H-90,3)
t(95,75,'FireGuard — updated complete circuit',62,True)
t(95,150,'ESP32-S3 N16R8 CAM + MQ-2 + MQ-135 + ADS1115 + BME280 + 3 buttons + active buzzer',32)
t(4700,90,'SHEET 1 / 1 • REV L',28,True,'rt');t(4700,135,'29 OCT 2026',27,False,'rt')
box(95,212,4610,78,3)
t(120,232,'TWO GAS CHANNELS + ENVIRONMENT + CAMERA/CNN. Hybrid location + optional station SMS use built-in Wi-Fi. No GPS module. CO unavailable.',32,True)

# Single 5 V power source and USB-C sink identification.
box(105,345,545,245)
t(130,367,'P1 — Apple A2246, 20 W',31,True)
notes(130,417,['5 V / 3 A at 200–240 V AC','USB-C cable → power breakout','Use the 5 V output.'],28,46)
ln([(650,465),(860,465)],6);t(680,382,'USB-C',27);t(680,420,'cable',27)
box(860,345,780,245);t(885,367,'J1 — HW-189 + CC PULL-DOWNS',29,True)
for yy,ref,cc in [(455,'R7','CC1'),(535,'R8','CC2')]:
    t(885,yy-17,cc,26,True);ln([(950,yy),(1085,yy)])
    box(1085,yy-15,74,30);ln([(1159,yy),(1450,yy)]);ground(1450,yy)
    t(1060,yy-38,ref+'  5.1 kΩ',25)
ln([(1640,430),(1760,430),(1760,620)]);dot(1760,620);t(1650,373,'VCC / VBUS',26,True)
ln([(1640,550),(1710,550)]);ground(1710,550);t(1650,506,'GND',26)
ln([(120,620),(4670,620)]);t(4290,570,'+5V MAIN RAIL',30,True)
dot(450,620);cap(450,620,780,'C3','100 µF / ≥10 V',True);ground(450,780)
dot(940,620);cap(940,620,780,'C4','100 nF');ground(940,780)
dot(2740,620);ln([(2740,620),(2740,470),(2860,470)])
for x in (2860,2950):ellipse(x-8,462,x+8,478,False,3)
ln([(2860,462),(2860,447),(2950,447),(2950,462)],6)
ln([(2950,470),(3060,470)]);label(3060,470,'+5V_ESP')
t(2755,365,'JP1 — REMOVABLE POWER LINK',29,True)
t(2755,413,'Disconnect header feed before PC USB.',27)
notes(3550,350,[
    '+5V_ESP → board header marked 5V.',
    'All GND points → board header GND.',
    'Keep board USB unplugged in this power mode.',
    'Power MQ modules from +5V, not ESP32 3V3.'
],27,43)

# Camera and control board. GPIO38 is retained for buzzer; heater output is removed.
box(2500,875,750,1305)
t(2540,920,'5V HEADER IN',28,True)
ln([(2690,875),(2690,830)]);label(2690,830,'+5V_ESP','left',25)
t(3025,920,'3V3 OUT',28,True,'rt');power(3075,875,'+3V3')
t(2600,1210,'U1 — ESP32-S3 CAM',39,True)
t(2600,1270,'N16R8 • attached 5640-marked camera',26)
t(2600,1320,'GPIO numbers, not header positions',27)
for y,s in [(1050,'GPIO41 / SDA'),(1170,'GPIO42 / SCL')]:t(2530,y-18,s,29,True)
for y,s in [(1500,'GPIO38 / BUZZ'),(1650,'GPIO14 / B1'),(1870,'GPIO47 / B2'),(2090,'GPIO21 / B3')]:t(3220,y-18,s,28,True,'rt')
notes(2600,1415,[
    'Camera stays in FPC socket.',
    'Camera images → CNN.',
    'No external camera wiring.',
    'Built-in Wi-Fi: hybrid location; no GPS.',
    'GPIO40 unconnected.',
    'MicroSD disabled.',
    'GPIO35–37: octal PSRAM.'
],26,52)
t(3150,2120,'GND',28,True,'rt');ln([(3100,2180),(3100,2210)]);ground(3100,2210)

# Two unmodified four-pin MQ modules and the two already-tested filter types.
for i,(name,y,adc) in enumerate([('MQ-2',1100,'A0'),('MQ-135',1810,'A2')]):
    box(200,y-90,460,240);t(230,y-12,'M'+str(i+1)+' — '+name,34,True)
    t(380,y-70,'VCC',25,False,'mt');ln([(380,y-90),(380,y-140)]);power(380,y-140,'+5V')
    t(620,y-20,'AO',26,False,'rt');ln([(660,y),(800,y)])
    t(620,y+83,'DO',25,False,'rt');ln([(660,y+100),(720,y+100)]);nc(720,y+100)
    t(380,y+105,'GND',25,False,'mt');ln([(380,y+150),(380,y+183)]);ground(380,y+183)
    rh(800,1120,y,'R'+str(i*2+1),'10 kΩ 1%')
    ln([(1120,y),(1600,y)]);dot(1190,y);dot(1430,y)
    rv(1190,y,y+190,'R'+str(i*2+2),'10 kΩ 1%',-1);ground(1190,y+190)
    cap(1430,y,y+190,'C'+str(i+1),'100 nF');ground(1430,y+190)
    t(820,y+270,'Use your tested divider/filter → '+adc,27,True)
    t(820,y+320,'Vout ≈ AO / 2; 5 V AO → 2.5 V',27)

# Preserve A0 / A2 so user labels and tested filters need no remapping.
box(1600,900,620,1290);t(1660,1300,'U2 — ADS1115',38,True)
t(1660,1360,'HW-198 • 3.3 V supply',28);t(1660,1410,'PGA target: ±4.096 V',27)
for y,s in [(1100,'A0 — MQ-2'),(1810,'A2 — MQ-135')]:t(1630,y-18,s,30,True)
ln([(1900,900),(1900,845)]);power(1900,845,'+3V3')
t(2190,1032,'SDA',27,False,'rt');t(2190,1152,'SCL',27,False,'rt')
t(2190,1582,'ADDR',27,False,'rt');ln([(2220,1600),(2300,1600),(2300,1640)]);ground(2300,1640)
t(1660,1655,'ADDR = GND → 0x48',27)
for y,s in [(1990,'A1'),(2090,'A3')]:
    t(2190,y-18,s,25,False,'rt');ln([(2220,y),(2280,y)]);nc(2280,y)
t(1660,2120,'ALRT: leave unconnected',26)
ln([(1900,2190),(1900,2220)]);ground(1900,2220)

# Shared I2C. Explicit hops show crossings without connections.
ln([(2220,1050),(2350,1050),(2350,790),(3400,790),(3400,1050)])
hop(2350,2500,1050,2420);dot(2350,1050)
ln([(2220,1170),(2420,1170),(2420,735),(3500,735),(3500,1170),(3650,1170)])
ln([(2420,1170),(2500,1170)]);dot(2420,1170)
hop(3400,3650,1050,3500);t(2700,680,'SHARED I²C BUS',28,True)
box(3650,900,760,390);t(3770,1230,'U3 — BME280',35,True)
t(3680,1032,'SDA',28);t(3680,1152,'SCL',28)
ln([(4000,900),(4000,850)]);power(4000,850,'+3V3');t(4015,920,'VCC',25)
t(4380,972,'CSB',25,False,'rt');ln([(4410,990),(4550,990)]);power(4550,990,'+3V3')
t(4380,1222,'SDO',25,False,'rt');ln([(4410,1240),(4630,1240),(4630,1270)]);ground(4630,1270)
t(4330,1100,'GND',27);ln([(4410,1140),(4510,1140),(4510,1180)]);ground(4510,1180)
notes(3660,1330,['CSB = 3.3 V; SDO = GND → 0x76.',
                  'Use fitted suitable pull-ups to 3.3 V.'],26,40)

# Three normally-open buttons; resistor-free input-pullup wiring.
for y,ref,desc in [(1650,'SW1','Mute / label'),(1870,'SW2','Readings / setup'),(2090,'SW3','Save frame / test')]:
    ln([(3250,y),(3510,y)])
    for x in (3510,3610):ellipse(x-7,y-7,x+7,y+7,False,3)
    ln([(3517,y-3),(3597,y-43)]);ln([(3617,y),(3720,y),(3720,y+40)]);ground(3720,y+40)
    t(3500,y-95,ref,28,True);t(3330,y+70,desc,26)
t(3330,2250,'Normally open; use internal pull-ups.',25)

# Known-spec replacement buzzer requirement, with existing PN2222A driver.
ln([(3250,1500),(3870,1500),(3870,1930),(3970,1930)])
rh(3970,4190,1930,'R5','1 kΩ');ln([(4190,1930),(4295,1930)]);dot(4220,1930)
rv(4220,1930,2150,'R6','100 kΩ',-1);ground(4220,2150)
ellipse(4260,1840,4460,2040,False,4)
ln([(4300,1880),(4300,1990)]);ln([(4295,1930),(4300,1930)])
ln([(4300,1895),(4390,1830),(4390,1750)])
ln([(4300,1975),(4390,2040),(4390,2150)]);arrow((4340,2005),(4390,2040));ground(4390,2150)
t(4480,1900,'Q1',29,True);t(4480,1945,'PN2222A',27)
t(4260,1865,'B',23);t(4410,1805,'C',23);t(4410,2040,'E',23)
power(4390,1530,'+5V');ln([(4390,1530),(4390,1570)])
# Buzzer transducer icon with + / - terminals.
d.arc((4330,1570,4450,1690),90,270,fill='black',width=4)
c.setLineWidth(4);c.arc(4330,H-1690,4450,H-1570,startAng=90,extent=180)
ln([(4390,1570),(4430,1570),(4430,1690),(4390,1690),(4390,1750)])
d.arc((4455,1560,4510,1700),285,75,fill='black',width=3)
c.setLineWidth(3);c.arc(4455,H-1700,4510,H-1560,startAng=-75,extent=150)
t(4030,1575,'BZ1 — NEW BUZZER',27,True)
t(4030,1620,'5 V DC ACTIVE',28,True);t(4030,1665,'2-pin; ≤20 mA',27)
t(4350,1535,'+',26);t(4350,1685,'−',26)
ln([(4390,1550),(4650,1550),(4650,1610)]);dot(4390,1550)
ln([(4620,1610),(4680,1610)])
polygon([(4650,1620),(4620,1680),(4680,1680)])
ln([(4650,1680),(4650,1750),(4390,1750)]);dot(4390,1750)
t(4515,1770,'D1  1N4007*',25);t(4500,1490,'K / stripe to +5V',23)
t(3900,2250,'* Clamp for inductive buzzer; use slow ON/OFF control.',24)

# Compact pin map and exact retained component counts.
ln([(90,2335),(4710,2335)],3)
for x in [1240,2400,3560]:ln([(x,2390),(x,2965)],2)
t(110,2390,'POWER / SHARED BUS',32,True)
notes(110,2460,[
    '+5V main → both MQ module VCC pins.',
    '+5V main → BZ1 positive; JP1 → ESP 5V.',
    'ESP 3V3 → ADS1115 VDD + BME VCC.',
    'GPIO41 → ADS SDA + BME SDA.',
    'GPIO42 → ADS SCL + BME SCL.',
    'All GND symbols connect together.',
    'CC1 → its own 5.1 kΩ → GND.',
    'CC2 → its own 5.1 kΩ → GND.',
    'No USB-A, OLED or SD wiring.'
],27,49)
t(1310,2390,'ADC / BME280 STRAPS',32,True)
notes(1310,2460,[
    'ADS A0 ← tested MQ-2 filter output.',
    'ADS A2 ← tested MQ-135 filter output.',
    'ADS A1, A3, ALRT: unconnected.',
    'ADS ADDR → GND; address 0x48.',
    'BME CSB → 3V3; SDO → GND.',
    'BME address 0x76; ID expected 0x60.',
    'Each filter: 10 kΩ series + 10 kΩ',
    'to GND; 100 nF output to GND.',
    'MQ digital outputs DO: unused.'
],27,49)
t(2470,2390,'BUTTONS / BUZZER DRIVER',32,True)
notes(2470,2460,[
    'GPIO14 → SW1 → GND.',
    'GPIO47 → SW2 → GND.',
    'GPIO21 → SW3 → GND.',
    'GPIO38 → 1 kΩ → Q1 base.',
    'Q1 base → 100 kΩ → GND.',
    'Q1 emitter → GND.',
    'Q1 collector → BZ1 negative.',
    'Q1 B/C/E are electrical terminals;',
    'match the actual PN2222A leadout.'
],27,49)
t(3630,2390,'PARTS RETAINED',32,True)
notes(3630,2460,[
    '10 kΩ 1% ×4; 5.1 kΩ ×2.',
    '1 kΩ ×1; 100 kΩ ×1.',
    '100 nF ×3; 100 µF (≥10 V) ×1.',
    'PN2222A ×1; 1N4007 ×1.',
    'Normally-open push buttons ×3.',
    '5 V active buzzer (≤20 mA) ×1.',
    'MQ-2 + MQ-135: one of each.',
    'ADS1115 + BME280: one of each.',
    'ESP32-S3 CAM + USB-C power input.'
],27,49)

ln([(90,3050),(4710,3050)],3)
t(110,3080,'REMOVED: MQ-7, its filter, LM2596, heater relay, Q2 driver and heater-control wiring. No CO channel in this revision.',29,True)
t(110,3130,'Reuse your tested MQ-2 and MQ-135 filters. A0 / A2 assignments are preserved. Never feed raw 5 V AO directly into the 3.3 V ADS1115.',27)
t(110,3180,'POWER: unplug the charger and disconnect the external 5 V header feed before connecting the ESP32 to a PC USB power source.',27)
t(110,3230,'LOCATION: saved installation pin first; optional cached Wi-Fi estimate. Station/test-number SMS via textbee; no extra wiring.',27)
t(110,3280,'STATUS: hybrid firmware integrated; geolocation and station-SMS switches default OFF. Validate assembled hardware and delivery.',28,True)

pngout=OUT/'FireGuard-updated-circuit.png'
im.save(pngout,dpi=(300,300))
im.resize((1920,1358)).save(WORK/'schematic-no-mq7-preview.png')
c.showPage();c.save()
reader=PdfReader(str(pdfout));assert len(reader.pages)==1
pdftext=reader.pages[0].extract_text()
for required in ['REV L','A0 — MQ-2','A2 — MQ-135','PN2222A','100 nF','NO CO']:
    if required=='NO CO':continue
    assert required in pdftext,required

# Preserve the previous image while updating the established current-image path.
canonical=OUT/'FireGuard-hardware-matched-schematic.png'
archive=WORK/'FireGuard-schematic-revI-archived.png'
if canonical.exists() and not archive.exists():shutil.copy2(canonical,archive)
shutil.copy2(pngout,canonical)

print(pngout);print(pdfout)
