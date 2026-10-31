"""Render the measured near-tie without exaggerating time differences.

Requires Pillow, FFmpeg and macOS Helvetica Neue/Menlo (or DejaVu fallback).
The code panel is static; each one-way leg lasts the measured run duration.
"""
import argparse,json,math,re,subprocess
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
OUT=ROOT/'build/billion-loops/video'
DATA=HERE.parent/'evidence/results.json'
if not DATA.exists(): DATA=ROOT/'build/billion-loops/results.json'
W,H,FPS,SECONDS,SCALE=1080,1350,60,8,2
BG='#09090b';FG='#f0eee9';MUTED='#96949b';ORANGE='#ff8057'
LANES=[('kalandra','Kalandra','-O2',ORANGE),('cpp','C++','-O3','#92bdff'),('rust','Rust','-O3','#d7c3ad')]
summary=json.loads(DATA.read_text())['summary']
SANS='/System/Library/Fonts/HelveticaNeue.ttc';MONO='/System/Library/Fonts/Menlo.ttc'
if not Path(SANS).exists():SANS='/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
if not Path(MONO).exists():MONO='/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf'
def font(size,mono=False,bold=False,scale=1):
 path=MONO if mono else SANS
 if path.endswith('.ttc'):return ImageFont.truetype(path,round(size*scale),index=int(bold))
 if bold:path=path.replace('.ttf','-Bold.ttf')
 return ImageFont.truetype(path,round(size*scale))

def make_base():
 image=Image.new('RGB',(W*SCALE,H*SCALE),BG);d=ImageDraw.Draw(image)
 def text(x,y,value,size=20,color=FG,mono=False,bold=False):
  d.text((x*SCALE,y*SCALE),value,font=font(size,mono,bold,SCALE),fill=color)
 def line(x1,y1,x2,y2,color,width=1):d.line(tuple(v*SCALE for v in (x1,y1,x2,y2)),fill=color,width=width*SCALE)
 def box(bounds,fill,outline=None,radius=0):
  b=tuple(round(v*SCALE) for v in bounds)
  if radius:d.rounded_rectangle(b,radius=radius*SCALE,fill=fill,outline=outline,width=SCALE)
  else:d.rectangle(b,fill=fill,outline=outline,width=SCALE)
 points=[(6,8),(86,8),(86,22),(55,22),(26,51),(6,51),(6,37),(20,37),(35,22),(6,22)]
 d.polygon([((48+x*.45)*SCALE,(40+y*.45)*SCALE) for x,y in points],fill=ORANGE)
 text(107,43,'KALANDRA / CPU BENCHMARK',17,ORANGE,True)
 text(48,92,'1 billion iterations.',57,bold=True)
 text(51,166,'10,000 × 100,000  ·  u = 40',23,MUTED)
 line(48,216,1032,216,'#38363b')
 text(50,231,'LANGUAGE / MEDIAN',14,MUTED,True)
 text(348,231,'START',14,MUTED,True)
 text(862,231,'1× REAL TIME',14,MUTED,True)
 for i,(key,name,flags,color) in enumerate(LANES):
  y=290+i*85
  if key=='kalandra':box((48,y-31,1032,y+39),'#17110f',radius=8)
  text(66,y-24,name,28,color,bold=True)
  text(66,y+11,f"{summary[key]['medianSeconds']*1000:.1f} ms  /  {flags}",18,MUTED,True)
  line(348,y+4,984,y+4,'#353339',2)
  for j in range(1,5):line(348+j*636/5,y-1,348+j*636/5,y+9,'#262429')
 line(984,266,984,482,'#c3bec7',1)
 text(48,511,'Effectively tied.',24,FG,bold=True)
 text(304,515,'Sub-percent differences are within noise.',19,MUTED)
 box((48,566,1032,1235),'#131315','#343137',12)
 text(74,587,'benchmark.k',20,FG,True)
 text(723,590,'EXPERIMENTAL APIs',15,ORANGE,True)
 line(49,629,1031,629,'#343137')
 source=(HERE/'display.k').read_text().splitlines()
 keywords={'fn','if','else'};types={'int','int32'};functions={'sumRemainders','main','generate'}
 token=re.compile(r'\b[A-Za-z_][A-Za-z_0-9]*\b|\b[0-9]+\b|->|[^A-Za-z_0-9]')
 for i,code in enumerate(source):
  y=648+i*29
  text(72,y+1,str(i+1).rjust(2),16,'#67636c',True)
  x=120
  for match in token.finditer(code):
   part=match.group();color=FG
   if part in keywords:color='#c4a5fb'
   elif part in types:color='#9ec8ff'
   elif part in functions or part in ('args','random'):color='#ffad8b'
   elif part.isdigit():color='#ddc49c'
   elif part=='->':color='#c4a5fb'
   elif part.strip() and not(part[0].isalnum() or part[0]=='_'):color='#a7a2ad'
   text(x,y,part,21.5,color,True)
   x+=d.textlength(part,font=font(21.5,True,False,SCALE))/SCALE
  assert x<1014,(i+1,x,'Code overflow')
 text(120,1166,'Output encodes r + result for verification.',14,MUTED)
 line(73,1190,1007,1190,'#302d34')
 text(74,1203,'$ ./benchmark 40 10000 100000',18,FG,True)
 text(48,1260,'Apple M2 Max  ·  7-run medians  ·  0.99.1 + experimental APIs',17,MUTED)
 text(48,1290,'Verified outputs  ·  Background OS maintenance active  ·  Effectively tied',15,MUTED)
 return image.resize((W,H),Image.Resampling.LANCZOS)

BASE=make_base()
def ball(color):
 size=48;im=Image.new('RGBA',(size*4,size*4));draw=ImageDraw.Draw(im)
 draw.ellipse((5*4,5*4,43*4,43*4),fill=color,outline='#eee8e0',width=2)
 return im.resize((size,size),Image.Resampling.LANCZOS)
BALLS={key:ball(color) for key,_,_,color in LANES}
def progress(elapsed, duration):
 # A triangle wave preserves constant speed through each out-and-back leg.
 phase=(elapsed/duration)%2
 return phase if phase<=1 else 2-phase

def frame(number):
 image=BASE.copy();d=ImageDraw.Draw(image)
 elapsed=number/FPS
 for i,(key,_,_,color) in enumerate(LANES):
  y=294+i*85;p=progress(elapsed,summary[key]['medianSeconds']);x=348+p*636
  if p>0:d.line((348,y,x,y),fill=color,width=3)
  image.paste(BALLS[key],(round(x)-24,y-24),BALLS[key])
 return image

def main():
 p=argparse.ArgumentParser();p.add_argument('--still',action='store_true');a=p.parse_args();OUT.mkdir(parents=True,exist_ok=True)
 frame(260).save(OUT/'mid-race.png');frame(460).save(OUT/'poster.png')
 if a.still:return
 output=OUT/'kalandra-billion-loops.mp4'
 cmd=['ffmpeg','-hide_banner','-loglevel','error','-y','-f','rawvideo','-pix_fmt','rgb24','-s',f'{W}x{H}','-r',str(FPS),'-i','-',
      '-an','-c:v','libx264','-preset','medium','-crf','17','-threads','2','-pix_fmt','yuv420p','-movflags','+faststart',str(output)]
 with subprocess.Popen(cmd,stdin=subprocess.PIPE) as process:
  for i in range(FPS*SECONDS):process.stdin.write(frame(i).tobytes())
  process.stdin.close();assert process.wait()==0
 print(output)
if __name__=='__main__':main()
