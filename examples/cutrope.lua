-- converted from TI-Nspire Lua by ti2prime (lib/ti.lua)
require("ti")

local function _atan2(y, x)                      -- 5.1 的 math.atan2(y, x)
  if x > 0 then return math.atan(y / x)
  elseif x < 0 then return math.atan(y / x) + (y >= 0 and math.pi or -math.pi)
  elseif y > 0 then return math.pi / 2
  elseif y < 0 then return -math.pi / 2
  else return 0 end
end
math.atan2 = _atan2

function on.create()
  --奥姆图像32*32
  --aom_image=image.new("图片")
  --糖果24*24
  --candy_image=image.new("")
  --选关按钮32*32
  --lvlsel_image=image.new("")
  --开始按钮
  --start_image=image.new("")
  --gc:drawImage(imagevar,x,y)
  --关卡(>20:绳  1:奥姆  2:糖果,每关10位)
  --3456:吹气(▲▶▼◀)  78:刺(↔↕)
  lvl_x = {161,137,160,0,0,0,0,0,0,0,66,122,210,191,97,0,0,0,0,0,220,146,159,213,192,0,0,0,0,0,153,56,216,149,231,142,0,0,0,0,0,0,0,0,0,0,0,0,0,0,175,18,272,97,0,0,0,0,0,0,155,136,156,153,0,0,0,0,0,0,151,151,193,205,103,99,199,151,0,0,17,144,258,268,95,0,0,0,0,0,157,156,102,213,141,0,0,0,0,0,171,79,169,140,213,173,38,0,0,0,10,131,130,266,123,0,0,0,0,0,10,123,78,237,87,264,0,0,0,0,93,93,11,119,0,0,0,0,0,0,161,159,160,65,212,61,0,0,0,0,189,124,222,120,57,0,0,0,0,0,169,172,276,277,210,85,64,224,0,0,69,8,71,118,198,254,0,0,0,0,62,8,136,98,77,135,206,281,0,0,72,3,135,273,78,0,0,0,0,0,72,7,121,236,249,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
  lvl_y = {20,177,92,0,0,0,0,0,0,0,13,10,14,140,58,0,0,0,0,0,20,21,111,176,66,0,0,0,0,0,15,86,89,154,171,84,0,0,0,0,0,0,0,0,0,0,0,0,0,0,95,120,82,100,0,0,0,0,0,0,71,174,124,159,0,0,0,0,0,0,21,147,55,113,47,111,182,87,0,0,195,103,25,173,146,0,0,0,0,0,24,96,75,135,175,0,0,0,0,0,38,44,198,198,195,167,16,0,0,0,90,10,191,94,96,0,0,0,0,0,106,166,14,21,95,142,0,0,0,0,9,82,77,172,0,0,0,0,0,0,106,158,199,151,54,6,0,0,0,0,20,109,161,28,175,0,0,0,0,0,93,207,96,202,15,156,19,151,0,0,17,72,83,180,182,84,0,0,0,0,66,119,91,184,126,14,18,96,0,0,23,90,89,171,104,0,0,0,0,0,14,70,68,1,179,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
  lvl_len_sty = {70,1,2,0,0,0,0,0,0,0,60,70,140,1,2,0,0,0,0,0,40,60,60,1,2,0,0,0,0,0,70,70,70,70,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,30,30,1,2,0,0,0,0,0,0,30,1,2,9,0,0,0,0,0,0,60,60,60,60,60,60,1,2,0,0,30,30,9,1,2,0,0,0,0,0,80,60,2,9,1,0,0,0,0,0,120,140,9,9,9,2,1,0,0,0,21,21,21,1,2,0,0,0,0,0,21,21,21,9,2,1,0,0,0,0,80,2,4,1,0,0,0,0,0,0,50,2,9,4,6,1,0,0,0,0,70,70,1,9,4,0,0,0,0,0,21,21,21,21,1,9,4,2,0,0,60,4,2,3,3,1,0,0,0,0,50,4,4,9,2,5,5,1,0,0,80,4,6,1,2,0,0,0,0,0,80,4,6,1,9,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
  px=150
  py=20
  ropel=50
  --绳子列表
  pxs={0,0,0,0,0,0,0,0,0,0}
  pys={0,0,0,0,0,0,0,0,0,0}
  rls={0,0,0,0,0,0,0,0,0,0}
  --length10
  rpn=10
  --obj 物体列表
  oxs={0,0,0,0,0,0,0,0,0,0}
  oys={0,0,0,0,0,0,0,0,0,0}
  obj={0,0,0,0,0,0,0,0,0,0}
  obl=10
  --奥姆
  ax=200
  ay=100
  --糖
  bx=200
  by=10
  bvx=0
  bvy=0
  --绳子中点
  tx=0
  ty=0
  --泡泡状态
  inbubble=0
  --绳子相关
  k=0.3
  f=1.01
  zta=0
  --计时器
  tmr=0
  
  num=nil
  char=0
  charn=0
  rk=0
   
  isropeb={0,0,0,0,0,0,0,0,0,0}
  isropeb_last={0,0,0,0,0,0,0,0,0,0}
  gamemode=0
  level=0
  timer.start(0.05)
end

function on.paint(gc)
  if gamemode==2 then
    ----奥姆
    gc:setColorRGB(0,255,0)
    gc:fillRect(ax,ay,32,32)
    gc:setColorRGB(0,0,0)
    ----绳子
    gc:setFont("sansserif","b",9)
    for i=1,rpn do
    px=pxs[i]
    py=pys[i]
    ropel=rls[i]
      if (by-py)^2+(bx-px)^2<ropel^2 then
        a=math.atan2(by-py,bx-px)
        dist=math.sqrt(ropel^2-((py-by)^2+(px-bx)^2))/2
        if px>bx then
          tx=(px+bx)/2+dist*math.sin(a)
          ty=(py+by)/2-dist*math.cos(a)
        else
          tx=(px+bx)/2-dist*math.sin(a)
          ty=(py+by)/2+dist*math.cos(a)
        end
        gc:drawLine(px,py,tx,ty)
        gc:drawLine(tx,ty,bx,by)
      else
        gc:drawLine(pxs[i],pys[i],bx,by)
      end
      gc:drawString(i,pxs[i]-4,pys[i],"middle")
    end
    ----杂物
    for i=1,obl do
    gc:setFont("sansserif","b",24)
    px=oxs[i]
    py=oys[i]
    ropel=obj[i]
    if ropel==3 then
      gc:drawString("▲",px,py)
    else
      if ropel==4 then
        gc:drawString("▶",px,py)
      else
        if ropel==5 then
          gc:drawString("▼",px,py)
        else
          if ropel==6 then
            gc:drawString("◀",px,py)
          else
            if ropel==9 then
              gc:drawArc(px-15,py-15,30,30,0,360)
              if (px-bx)^2+(py-by)^2<=256 then
                inbubble=1
                table.remove(oxs,i)
                table.remove(oys,i)
                table.remove(obj,i)
                obl=obl-1
              end
            end
          end
        end
      end
    end
    gc:setFont("sansserif","b",10)
    gc:drawString(string.uchar(64+i),px-9,py)
    end
    ----糖
    gc:setColorRGB(128,50,0)
    gc:fillArc(bx-12,by-12,24,24,0,360)
    gc:setColorRGB(0,0,0)
    if inbubble==1 then
      gc:drawArc(bx-15,by-15,30,30,0,360)
      gc:setFont("sansserif","b",9)
      gc:drawString("Z",bx-17,by-19)
    end
  else if gamemode==1 then
    local a=0
    for i=0,3 do
      for j=0,7 do
        a=a+1
        gc:setColorRGB(0,100,255)
        gc:fillRect(j*40+3,i*40+8,32,32)
        gc:setColorRGB(255,255,255)
        gc:setFont("sansserif","b",12)
        gc:drawString(a,j*40+12,i*40+14)
      end
    end
    gc:setColorRGB(0,0,0)
    gc:drawRect(math.fmod(level,8)*40+1,math.floor(level/8)*40+6,35,35)
    gc:drawRect(math.fmod(level,8)*40,math.floor(level/8)*40+5,37,37)
    gc:drawString("Press [esc] to escape",10,170)
  else if gamemode==0 then
    gc:setColorRGB(0,0,0)
    gc:setFont("sansserif","b",24)
    gc:drawString("Cut the Rope",50,20)
    gc:setFont("sansserif","b",16)
    gc:drawString("Press ENTER",90,120)
  else if gamemode==3 then
    gc:setColorRGB(0,0,0)
    gc:setFont("sansserif","r",24)
    gc:drawString("Loading......",70,70)
  else if gamemode==4 then
    gc:setColorRGB(0,0,0)
    gc:setFont("sansserif","b",24)
    gc:drawString("CLEAR!!!",80,20)
    gc:setFont("sansserif","b",16)
    gc:drawString("1:Next Level",80,80)
    gc:drawString("2:Restart",80,120)
    gc:drawString("esc:Escape",10,170)
  end
  end
  end
  end
  end
  timer.start(0.05)
end

function on.charIn(ch)
  num=string.byte(ch)-48
  char=ch
end

function on.escapeKey()
  rk="esc"
end

function on.enterKey()
  rk="enter"
end

function on.mouseDown()
  rk="enter"
end

function on.arrowKey(key)
  rk=key
end

function on.timer()
  --按键
  if gamemode==2 then
    if rk=="esc" then
      gamemode=1
      rk=0
    end
    if num and num>=0 and num<=rpn then
      table.remove(pxs,num)
      table.remove(pys,num)
      table.remove(rls,num)
      rpn=rpn-1
      num=nil
    end
    --物体obj逻辑
    if bx-ax>=0 and bx-ax<=32 and by-ay>=0 and by-ay<=32 then
      gamemode=4
    end
    if by<-16 then
      inbubble=0
    end
    charn=string.byte(char)-96
    if charn>=1 and charn<=obl then
      ropel=obj[charn]
      px=oxs[charn]
      py=oys[charn]
      if ropel==3 then
        if bx-px>=0 and bx-px<=24 and by-py>=-226 and by-py<=24 then
          bvy=bvy-6
        end
      else
        if ropel==4 then
          if bx-px>=0 and bx-px<=250 and by-py>=0 and by-py<=24 then
            bvx=bvx+6  
          end
        else
          if ropel==5 then
            if bx-px>=0 and bx-px<=24 and by-py>=0 and by-py<=250 then
              bvy=bvy+6
            end
          else
            if ropel==6 then
              if bx-px>=-226 and bx-px<=24 and by-py>=0 and by-py<=24 then
                bvx=bvx-6
              end 
            end
          end
        end
      end
      charn=0
      char=0
    end
      if char=="z" then
        inbubble=0
        char=0
      end
    bx=bx+bvx
    by=by+bvy-2*inbubble
    bvx=bvx/(f+0.5*inbubble)
    bvy=bvy/(f+0.5*inbubble)+1*(1-inbubble)
    for i=1,rpn do
      px=pxs[i]
      py=pys[i]
      ropel=rls[i]
      zta=math.atan2(by-py,bx-px)
      if (by-py)^2+(bx-px)^2>ropel^2 then
        isropeb[i]=1
        dist=math.sqrt((py-by)^2+(px-bx)^2)
        bvy=bvy-(dist-ropel)*k*math.sin(zta)
        bvx=bvx-(dist-ropel)*k*math.cos(zta)
--        if isropeb_last[i]==0 then
--          bvy=bvy-0.2*(dist-ropel)*k*math.sin(zta)
--          bvx=bvx-0.2*(dist-ropel)*k*math.cos(zta)
--        end
      else
        isropeb[i]=0
      end
        isropeb_last[i]=isropeb[i]
    end
    if bvx>20 then bvx=20 end
    if bvx<-20 then bvx=-20 end
    if bvy>20 then bvy=20 end
    if bvy<-20 then bvy=-20 end
    if by>216 then
      gamemode=3
    end
  else if gamemode==1 then
    if rk=="enter" then
      gamemode=3
      rk=0
    else
      if rk=="right" then
        level=level+1
        rk=0
      else
        if rk=="left" then
          level=level-1
          rk=0
        else
          if rk=="up" then
            level=level-8
            rk=0
          else
            if rk=="down" then
              level=level+8
              rk=0
            else
              if rk=="esc" then
                gamemode=0
                rk=0
              end
            end
          end
        end
      end
    end
    if level>31 then level=level-32 end
    if level<0 then level=level+32 else  end
  else if gamemode==0 then
    if rk=="enter" then
      gamemode=1
      rk=0
    end
  else if gamemode==3 then
    platform.window:invalidate()
    
    rpn=0
    char=0
    charn=0
    rk=0
    inbubble=0
    pxs={0,0,0,0,0,0,0,0,0,0}
    pys={0,0,0,0,0,0,0,0,0,0}
    rls={0,0,0,0,0,0,0,0,0,0}
    repeat 
      rpn=rpn+1
      pxs[rpn]=lvl_x[rpn+10*level]
      pys[rpn]=lvl_y[rpn+10*level]
      rls[rpn]=lvl_len_sty[rpn+10*level]
    until lvl_len_sty[rpn+10*level]<=20
    rpn=rpn-1
    bvx=0
    bvy=0
    bx=180
    by=108
    ax=160
    ay=118
    oxs={0,0,0,0,0,0,0,0,0,0}
    oys={0,0,0,0,0,0,0,0,0,0}
    obj={0,0,0,0,0,0,0,0,0,0}
    obl=0
    for i=rpn+1,10 do
      if lvl_len_sty[i+10*level]==1 then
        ax=lvl_x[i+10*level]
        ay=lvl_y[i+10*level]
      else
        if lvl_len_sty[i+10*level]==2 then
          bx=lvl_x[i+10*level]
          by=lvl_y[i+10*level]
        else
          if lvl_len_sty[i+10*level] ~= 0 then
            oxs[obl+1]=lvl_x[i+10*level]
            oys[obl+1]=lvl_y[i+10*level]
            obj[obl+1]=lvl_len_sty[i+10*level]
            obl=obl+1
          end
        end
      end
    end
    num=nil
    gamemode=2
  else if gamemode==4 then
    if num==1 then
      if level>=31 then
        gamemode=1
      else
        level=level+1
        gamemode=3
      end
      num=nil
      rk=0
     else
       if num==2 then
        gamemode=3
        num=nil
        rk=0  
       else
         if rk=="esc" then
           gamemode=1
           num=nil
           rk=0
         end
       end
    end
  end
  end
  end
  end
  end
  platform.window:invalidate()
end

ti.run()
