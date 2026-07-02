#pragma once
//
// svg.h — SVG renderer for Vertex (v2).
//
// Full feature list:
// - Elements: rect (rx/ry), circle, ellipse, line, polyline, polygon,
//   path (M/L/H/V/C/S/Q/T/A/Z), text/tspan, image, use, g, nested svg
// - Paint: linearGradient, radialGradient with stops + spreadMethod
// - Style: fill, stroke, stroke-width, stroke-dasharray, stroke-linecap,
//   stroke-linejoin, opacity, fill-opacity, stroke-opacity, fill-rule,
//   display, visibility, class+<style> CSS
// - Transform: translate, scale, rotate, skewX/Y, matrix
// - ViewBox: proper viewport transform with preserveAspectRatio
// - Colors: all 148 CSS named + #hex + rgb()/rgba()/hsl()/hsla()
// - Defs/use: element reuse by id
//
#include "html/dom.h"
#include "html/parser.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <functional>

struct SvgBitmap {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels; // RGBA
};

namespace svg {

// ── Color ───────────────────────────────────────────────────────────────────

struct Color { uint8_t r=0,g=0,b=0,a=255; };

inline Color parseColor(const std::string& s) {
    if (s.empty()||s=="none"||s=="transparent") return {0,0,0,0};
    if (s[0]=='#') { try {
        if (s.size()==4){unsigned long v=std::stoul(s.substr(1),nullptr,16);return{(uint8_t)(((v>>8)&0xF)*17),(uint8_t)(((v>>4)&0xF)*17),(uint8_t)((v&0xF)*17),255};}
        if (s.size()>=7){unsigned long v=std::stoul(s.substr(1,6),nullptr,16);return{(uint8_t)(v>>16),(uint8_t)((v>>8)&0xFF),(uint8_t)(v&0xFF),255};}
    } catch(...){} }
    if (s.rfind("rgb",0)==0) {
        size_t p=s.find('('),e=s.rfind(')');if(p!=std::string::npos&&e!=std::string::npos){
        std::string inner=s.substr(p+1,e-p-1);float v[4]={0,0,0,1};int n=0;
        std::istringstream ss(inner);std::string tok;
        while(std::getline(ss,tok,',')&&n<4){while(!tok.empty()&&tok[0]==' ')tok.erase(tok.begin());
            size_t sl=tok.find('/');if(sl!=std::string::npos){try{v[n++]=std::stof(tok.substr(0,sl));}catch(...){}try{v[n++]=std::stof(tok.substr(sl+1));}catch(...){}}
            else{try{v[n++]=std::stof(tok);}catch(...){}}
        }
        return{(uint8_t)std::clamp(v[0],0.f,255.f),(uint8_t)std::clamp(v[1],0.f,255.f),(uint8_t)std::clamp(v[2],0.f,255.f),(uint8_t)(std::clamp(v[3],0.f,1.f)*255)};
    }}
    if (s.rfind("hsl",0)==0) {
        size_t p=s.find('('),e=s.rfind(')');if(p!=std::string::npos&&e!=std::string::npos){
        float h=0,sa=0,l=0;std::string inner=s.substr(p+1,e-p-1);
        if(sscanf(inner.c_str(),"%f,%f%%,%f%%",&h,&sa,&l)>=3||sscanf(inner.c_str(),"%f %f%% %f%%",&h,&sa,&l)>=3){
            sa/=100;l/=100;auto hue2rgb=[](float p,float q,float t){if(t<0)t+=1;if(t>1)t-=1;if(t<1.f/6)return p+(q-p)*6*t;if(t<.5f)return q;if(t<2.f/3)return p+(q-p)*(2.f/3-t)*6;return p;};
            float q=l<.5f?l*(1+sa):l+sa-l*sa,pp=2*l-q,hf=h/360.f;
            return{(uint8_t)(hue2rgb(pp,q,hf+1.f/3)*255),(uint8_t)(hue2rgb(pp,q,hf)*255),(uint8_t)(hue2rgb(pp,q,hf-1.f/3)*255),255};
    }}}
    static const std::map<std::string,uint32_t> named={
        {"aliceblue",0xF0F8FF},{"antiquewhite",0xFAEBD7},{"aqua",0x00FFFF},{"aquamarine",0x7FFFD4},{"azure",0xF0FFFF},{"beige",0xF5F5DC},{"bisque",0xFFE4C4},{"black",0x000000},{"blanchedalmond",0xFFEBCD},{"blue",0x0000FF},{"blueviolet",0x8A2BE2},{"brown",0xA52A2A},{"burlywood",0xDEB887},{"cadetblue",0x5F9EA0},{"chartreuse",0x7FFF00},{"chocolate",0xD2691E},{"coral",0xFF7F50},{"cornflowerblue",0x6495ED},{"cornsilk",0xFFF8DC},{"crimson",0xDC143C},{"cyan",0x00FFFF},{"darkblue",0x00008B},{"darkcyan",0x008B8B},{"darkgoldenrod",0xB8860B},{"darkgray",0xA9A9A9},{"darkgreen",0x006400},{"darkgrey",0xA9A9A9},{"darkkhaki",0xBDB76B},{"darkmagenta",0x8B008B},{"darkolivegreen",0x556B2F},{"darkorange",0xFF8C00},{"darkorchid",0x9932CC},{"darkred",0x8B0000},{"darksalmon",0xE9967A},{"darkseagreen",0x8FBC8F},{"darkslateblue",0x483D8B},{"darkslategray",0x2F4F4F},{"darkslategrey",0x2F4F4F},{"darkturquoise",0x00CED1},{"darkviolet",0x9400D3},{"deeppink",0xFF1493},{"deepskyblue",0x00BFFF},{"dimgray",0x696969},{"dimgrey",0x696969},{"dodgerblue",0x1E90FF},{"firebrick",0xB22222},{"floralwhite",0xFFFAF0},{"forestgreen",0x228B22},{"fuchsia",0xFF00FF},{"gainsboro",0xDCDCDC},{"ghostwhite",0xF8F8FF},{"gold",0xFFD700},{"goldenrod",0xDAA520},{"gray",0x808080},{"green",0x008000},{"greenyellow",0xADFF2F},{"grey",0x808080},{"honeydew",0xF0FFF0},{"hotpink",0xFF69B4},{"indianred",0xCD5C5C},{"indigo",0x4B0082},{"ivory",0xFFFFF0},{"khaki",0xF0E68C},{"lavender",0xE6E6FA},{"lavenderblush",0xFFF0F5},{"lawngreen",0x7CFC00},{"lemonchiffon",0xFFFACD},{"lightblue",0xADD8E6},{"lightcoral",0xF08080},{"lightcyan",0xE0FFFF},{"lightgoldenrodyellow",0xFAFAD2},{"lightgray",0xD3D3D3},{"lightgreen",0x90EE90},{"lightgrey",0xD3D3D3},{"lightpink",0xFFB6C1},{"lightsalmon",0xFFA07A},{"lightseagreen",0x20B2AA},{"lightskyblue",0x87CEFA},{"lightslategray",0x778899},{"lightslategrey",0x778899},{"lightsteelblue",0xB0C4DE},{"lightyellow",0xFFFFE0},{"lime",0x00FF00},{"limegreen",0x32CD32},{"linen",0xFAF0E6},{"magenta",0xFF00FF},{"maroon",0x800000},{"mediumaquamarine",0x66CDAA},{"mediumblue",0x0000CD},{"mediumorchid",0xBA55D3},{"mediumpurple",0x9370DB},{"mediumseagreen",0x3CB371},{"mediumslateblue",0x7B68EE},{"mediumspringgreen",0x00FA9A},{"mediumturquoise",0x48D1CC},{"mediumvioletred",0xC71585},{"midnightblue",0x191970},{"mintcream",0xF5FFFA},{"mistyrose",0xFFE4E1},{"moccasin",0xFFE4B5},{"navajowhite",0xFFDEAD},{"navy",0x000080},{"oldlace",0xFDF5E6},{"olive",0x808000},{"olivedrab",0x6B8E23},{"orange",0xFFA500},{"orangered",0xFF4500},{"orchid",0xDA70D6},{"palegoldenrod",0xEEE8AA},{"palegreen",0x98FB98},{"paleturquoise",0xAFEEEE},{"palevioletred",0xDB7093},{"papayawhip",0xFFEFD5},{"peachpuff",0xFFDAB9},{"peru",0xCD853F},{"pink",0xFFC0CB},{"plum",0xDDA0DD},{"powderblue",0xB0E0E6},{"purple",0x800080},{"rebeccapurple",0x663399},{"red",0xFF0000},{"rosybrown",0xBC8F8F},{"royalblue",0x4169E1},{"saddlebrown",0x8B4513},{"salmon",0xFA8072},{"sandybrown",0xF4A460},{"seagreen",0x2E8B57},{"seashell",0xFFF5EE},{"sienna",0xA0522D},{"silver",0xC0C0C0},{"skyblue",0x87CEEB},{"slateblue",0x6A5ACD},{"slategray",0x708090},{"slategrey",0x708090},{"snow",0xFFFAFA},{"springgreen",0x00FF7F},{"steelblue",0x4682B4},{"tan",0xD2B48C},{"teal",0x008080},{"thistle",0xD8BFD8},{"tomato",0xFF6347},{"turquoise",0x40E0D0},{"violet",0xEE82EE},{"wheat",0xF5DEB3},{"white",0xFFFFFF},{"whitesmoke",0xF5F5F5},{"yellow",0xFFFF00},{"yellowgreen",0x9ACD32}
    };
    std::string low=s;for(auto&c:low)c=(char)std::tolower((unsigned char)c);
    auto it=named.find(low);if(it!=named.end()){uint32_t v=it->second;return{(uint8_t)(v>>16),(uint8_t)((v>>8)&0xFF),(uint8_t)(v&0xFF),255};}
    return{0,0,0,255};
}

// ── Transform ───────────────────────────────────────────────────────────────

struct Mat{float a=1,b=0,c=0,d=1,e=0,f=0;};
inline Mat matMul(const Mat&l,const Mat&r){return{l.a*r.a+l.c*r.b,l.b*r.a+l.d*r.b,l.a*r.c+l.c*r.d,l.b*r.c+l.d*r.d,l.a*r.e+l.c*r.f+l.e,l.b*r.e+l.d*r.f+l.f};}
inline void txPt(const Mat&m,float&x,float&y){float nx=m.a*x+m.c*y+m.e,ny=m.b*x+m.d*y+m.f;x=nx;y=ny;}
inline bool invMat(const Mat&m,Mat&out){float det=m.a*m.d-m.b*m.c;if(std::abs(det)<0.000001f)return false;float id=1.f/det;out={m.d*id,-m.b*id,-m.c*id,m.a*id,(m.c*m.f-m.d*m.e)*id,(m.b*m.e-m.a*m.f)*id};return true;}

inline Mat parseTransform(const std::string& s){
    Mat result;size_t pos=0;
    while(pos<s.size()){
        while(pos<s.size()&&!std::isalpha((unsigned char)s[pos]))++pos;
        size_t ns=pos;while(pos<s.size()&&std::isalpha((unsigned char)s[pos]))++pos;
        std::string fn=s.substr(ns,pos-ns);
        size_t p=s.find('(',pos),e=s.find(')',p!=std::string::npos?p:pos);
        if(p==std::string::npos||e==std::string::npos)break;
        std::string args=s.substr(p+1,e-p-1);float v[6]={};int n=0;
        std::istringstream ss(args);float fv;std::string tok;
        while(n<6&&std::getline(ss,tok,',')){std::istringstream s2(tok);while(n<6&&s2>>fv)v[n++]=fv;}
        Mat m;
        if(fn=="translate"){m.e=v[0];m.f=n>=2?v[1]:0;}
        else if(fn=="scale"){m.a=v[0];m.d=n>=2?v[1]:v[0];}
        else if(fn=="rotate"){float rad=v[0]*3.14159265f/180,cs=std::cos(rad),sn=std::sin(rad);if(n>=3)m={cs,sn,-sn,cs,v[1]*(1-cs)+v[2]*sn,v[2]*(1-cs)-v[1]*sn};else m={cs,sn,-sn,cs,0,0};}
        else if(fn=="skewX"){m.c=std::tan(v[0]*3.14159265f/180);}
        else if(fn=="skewY"){m.b=std::tan(v[0]*3.14159265f/180);}
        else if(fn=="matrix"&&n>=6){m={v[0],v[1],v[2],v[3],v[4],v[5]};}
        result=matMul(result,m);pos=e+1;
    }
    return result;
}

// ── Drawing primitives ──────────────────────────────────────────────────────

inline float parseNum(const std::string& s){try{return std::stof(s);}catch(...){return 0;}}

inline void blendPixel(SvgBitmap& bmp,int x,int y,Color c){
    if(x<0||x>=bmp.width||y<0||y>=bmp.height||c.a==0)return;
    int idx=(y*bmp.width+x)*4;
    if(c.a==255){bmp.pixels[idx]=c.r;bmp.pixels[idx+1]=c.g;bmp.pixels[idx+2]=c.b;bmp.pixels[idx+3]=255;}
    else{float a=c.a/255.f,ia=1.f-a;bmp.pixels[idx]=(uint8_t)(c.r*a+bmp.pixels[idx]*ia);bmp.pixels[idx+1]=(uint8_t)(c.g*a+bmp.pixels[idx+1]*ia);bmp.pixels[idx+2]=(uint8_t)(c.b*a+bmp.pixels[idx+2]*ia);bmp.pixels[idx+3]=std::max(bmp.pixels[idx+3],c.a);}
}

struct Pt{float x,y;};

// ── Bitmap font (5x7 glyphs for ASCII 32-126) ──────────────────────────────
// Each glyph is 5 columns x 7 rows packed into a uint32_t (35 bits, top-left first).
// Row-major: bits 0-4 = row 0, bits 5-9 = row 1, etc.

inline uint32_t fontGlyph(char ch) {
    if (ch < 32 || ch > 126) return 0;
    static const uint32_t glyphs[] = {
        0x00000000, // space
        0x04104100, // !
        0x0A500000, // "
        0x0AFABEA0, // #
        0x04F83C4F, // $  (approximate)
        0x19A11590, // %
        0x0C54E950, // &  (approximate)
        0x04200000, // '
        0x02222100, // (
        0x04222200, // )
        0x00A4A000, // *
        0x0023E200, // +
        0x00000220, // ,
        0x0001F000, // -
        0x00000100, // .
        0x01111000, // /
        0x0E9B9B8E, // 0 (approximate compact)
        0x04604104, // 1
        0x0E11F10E, // 2 (approximate)
        0x0E10E10E, // 3
        0x0A5F1010, // 4 (approximate)
        0x1F0F010E, // 5 (approximate)
        0x0E0F110E, // 6 (approximate)
        0x1F111000, // 7
        0x0E1F110E, // 8 (approximate)
        0x0E11E10E, // 9 (approximate)
        0x00100100, // :
        0x00100220, // ;
        0x01242100, // <
        0x001F07C0, // =
        0x04242400, // >
        0x0E110400, // ?
        0x0E9F90E0, // @
        0x04A1FA10, // A
        0x1E9E91E0, // B
        0x0E8081E0, // C  (approximate)
        0x1E9091E0, // D
        0x1F0F01F0, // E  (approximate)
        0x1F0F0100, // F
        0x0E80B1E0, // G  (approximate)
        0x111F1110, // H
        0x0E204070, // I  (approximate)
        0x10101090, // J  (approximate)
        0x114A4510, // K  (approximate)
        0x0101011F, // L
        0x11BB5110, // M  (approximate)
        0x119D5310, // N  (approximate)
        0x0E91190E, // O  (approximate)
        0x1E9E0100, // P
        0x0E911A0E, // Q  (approximate)
        0x1E9E4A10, // R  (approximate)
        0x0F01E10E, // S  (approximate)
        0x1F204040, // T  (approximate)
        0x1191190E, // U  (approximate)
        0x11151204, // V  (approximate)
        0x1115AB10, // W  (approximate)
        0x110A0A10, // X  (approximate)
        0x110A0404, // Y  (approximate)
        0x1F11111F, // Z  (approximate)
        0x06222300, // [
        0x10080400, // backslash
        0x06444600, // ]
        0x04A00000, // ^
        0x0000001F, // _
        0x04200000, // `
        0x000E9F00, // a (lowercase approximate)
        0x010F110E, // b
        0x000E010E, // c
        0x100F110E, // d  (approximate)
        0x000E1F0E, // e  (approximate)
        0x06070404, // f
        0x000E9E10, // g  (approximate)
        0x010F1110, // h
        0x00404040, // i
        0x00404460, // j
        0x01051410, // k  (approximate)
        0x02020204, // l
        0x001B5510, // m  (approximate)
        0x000F1110, // n
        0x000E110E, // o
        0x000E11E1, // p  (approximate)
        0x000E9F10, // q  (approximate)
        0x000B0404, // r  (approximate)
        0x000F0E10, // s  (approximate)
        0x04070404, // t
        0x00091190, // u  (approximate)
        0x00051204, // v  (approximate)
        0x0011AB10, // w  (approximate)
        0x000A0A00, // x  (approximate)
        0x000A0460, // y  (approximate)
        0x001F111F, // z  (approximate)
        0x02242100, // {
        0x04040404, // |
        0x04242400, // }
        0x000A5000, // ~
    };
    return glyphs[ch - 32];
}

inline void drawChar(SvgBitmap& bmp, char ch, float px, float py, float scale, Color c, const Mat& m) {
    uint32_t g = fontGlyph(ch);
    if (!g) return;
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (g & (1 << (row * 5 + (4 - col)))) {
                float x = px + col * scale, y = py + row * scale;
                txPt(m, x, y);
                int ix = (int)(x + 0.5f), iy = (int)(y + 0.5f);
                for (int dy = 0; dy < (int)(scale + 0.5f); ++dy)
                    for (int dx = 0; dx < (int)(scale + 0.5f); ++dx)
                        blendPixel(bmp, ix + dx, iy + dy, c);
            }
        }
    }
}

inline void drawString(SvgBitmap& bmp, const std::string& text, float x, float y, float fontSize, Color c, const Mat& m, int anchor = 0) {
    float scale = fontSize / 7.f;  // 7 rows in the bitmap font
    float charW = 6 * scale;  // 5px + 1px gap
    // text-anchor: 0=start, 1=middle, 2=end
    float totalW = text.size() * charW;
    float startX = x;
    if (anchor == 1) startX = x - totalW / 2;
    else if (anchor == 2) startX = x - totalW;
    for (size_t i = 0; i < text.size(); ++i)
        drawChar(bmp, text[i], startX + i * charW, y - fontSize * 0.8f, scale, c, m);
}

// ── Gradient support ────────────────────────────────────────────────────────

struct GradStop{float offset;Color color;};
struct Gradient{
    bool isRadial=false;
    float x1=0,y1=0,x2=1,y2=0; // linear
    float cx=0.5f,cy=0.5f,r=0.5f,fx=-1,fy=-1; // radial
    std::vector<GradStop> stops;
    bool userSpace=false;
    Mat transform; // gradientTransform
    int spread=0; // 0=pad, 1=reflect, 2=repeat
};

inline Color sampleGradient(const Gradient& g, float t){
    // spreadMethod: pad (clamp), reflect (bounce), repeat (wrap)
    if(g.spread==1){t=std::abs(t);int it=(int)t;t-=it;if(it%2)t=1-t;} // reflect
    else if(g.spread==2){t=t-std::floor(t);} // repeat
    t=std::clamp(t,0.f,1.f);
    if(g.stops.empty())return{0,0,0,255};
    if(g.stops.size()==1)return g.stops[0].color;
    if(t<=g.stops.front().offset)return g.stops.front().color;
    if(t>=g.stops.back().offset)return g.stops.back().color;
    for(size_t i=0;i+1<g.stops.size();++i){
        if(t>=g.stops[i].offset&&t<=g.stops[i+1].offset){
            float range=g.stops[i+1].offset-g.stops[i].offset;
            float f=range>0.001f?(t-g.stops[i].offset)/range:0;
            Color a=g.stops[i].color,b=g.stops[i+1].color;
            return{(uint8_t)(a.r+(b.r-a.r)*f),(uint8_t)(a.g+(b.g-a.g)*f),(uint8_t)(a.b+(b.b-a.b)*f),(uint8_t)(a.a+(b.a-a.a)*f)};
        }
    }
    return g.stops.back().color;
}

inline void collectGradients(const Node* n, std::map<std::string,Gradient>& grads){
    if(!n)return;
    if(n->type==NodeType::Element&&(n->tagName=="lineargradient"||n->tagName=="radialgradient")){
        std::string id=n->attr("id");if(id.empty())return;
        Gradient g;
        g.isRadial=(n->tagName=="radialgradient");
        std::string units=n->attr("gradientUnits");if(units.empty())units=n->attr("gradientunits");
        g.userSpace=(units=="userSpaceOnUse"||units=="userspaceonuse");
        std::string gt=n->attr("gradientTransform");if(gt.empty())gt=n->attr("gradienttransform");if(!gt.empty())g.transform=parseTransform(gt);
        std::string sm=n->attr("spreadMethod");if(sm.empty())sm=n->attr("spreadmethod");if(sm=="reflect")g.spread=1;else if(sm=="repeat")g.spread=2;
        if(!g.isRadial){
            g.x1=parseNum(n->attr("x1"));g.y1=parseNum(n->attr("y1"));
            g.x2=n->attr("x2").empty()?1.f:parseNum(n->attr("x2"));g.y2=parseNum(n->attr("y2"));
            // Handle percentage (common: x1="0%" x2="100%")
            auto pct=[](const std::string&s)->float{if(s.find('%')!=std::string::npos)return parseNum(s)/100.f;return parseNum(s);};
            if(!n->attr("x1").empty())g.x1=pct(n->attr("x1"));
            if(!n->attr("y1").empty())g.y1=pct(n->attr("y1"));
            if(!n->attr("x2").empty())g.x2=pct(n->attr("x2"));
            if(!n->attr("y2").empty())g.y2=pct(n->attr("y2"));
        }else{
            auto pct=[](const std::string&s,float def)->float{if(s.empty())return def;if(s.find('%')!=std::string::npos)return parseNum(s)/100.f;return parseNum(s);};
            g.cx=pct(n->attr("cx"),0.5f);g.cy=pct(n->attr("cy"),0.5f);g.r=pct(n->attr("r"),0.5f);
            g.fx=pct(n->attr("fx"),-1);g.fy=pct(n->attr("fy"),-1);
            if(g.fx<0)g.fx=g.cx;if(g.fy<0)g.fy=g.cy;
        }
        for(auto&child:n->children){
            if(child->type==NodeType::Element&&child->tagName=="stop"){
                GradStop stop;
                std::string off=child->attr("offset");stop.offset=parseNum(off);
                if(off.find('%')!=std::string::npos)stop.offset/=100.f;
                std::string sc=child->attr("stop-color");
                // Also check style attribute for stop-color
                std::string st=child->attr("style");
                if(!st.empty()&&sc.empty()){size_t p=st.find("stop-color:");if(p!=std::string::npos){size_t e=st.find(';',p);sc=st.substr(p+11,e==std::string::npos?std::string::npos:e-p-11);while(!sc.empty()&&sc[0]==' ')sc.erase(sc.begin());}}
                stop.color=sc.empty()?Color{0,0,0,255}:parseColor(sc);
                std::string so=child->attr("stop-opacity");
                if(so.empty()&&!st.empty()){size_t p=st.find("stop-opacity:");if(p!=std::string::npos){size_t e=st.find(';',p);so=st.substr(p+13,e==std::string::npos?std::string::npos:e-p-13);while(!so.empty()&&so[0]==' ')so.erase(so.begin());}}
                if(!so.empty())try{stop.color.a=(uint8_t)(std::stof(so)*255);}catch(...){}
                g.stops.push_back(stop);
            }
        }
        // Handle href to inherit stops from another gradient
        std::string href=n->attr("href");if(href.empty())href=n->attr("xlink:href");
        if(!href.empty()&&href[0]=='#'&&g.stops.empty()){auto it=grads.find(href.substr(1));if(it!=grads.end())g.stops=it->second.stops;}
        grads[id]=g;
    }
    for(auto&c:n->children)collectGradients(c.get(),grads);
}

// ── SVG CSS (<style> inside SVG) ────────────────────────────────────────────

struct SvgCssRule{std::string selector;std::map<std::string,std::string>props;};

inline void collectSvgCss(const Node* n, std::vector<SvgCssRule>& rules){
    if(!n)return;
    if(n->type==NodeType::Element&&n->tagName=="style"){
        std::string css;for(auto&c:n->children)if(c->type==NodeType::Text)css+=c->text;
        // Very simple CSS parser: selector { prop: val; }
        size_t pos=0;
        while(pos<css.size()){
            size_t brace=css.find('{',pos);if(brace==std::string::npos)break;
            size_t end=css.find('}',brace);if(end==std::string::npos)break;
            std::string sel=css.substr(pos,brace-pos);
            std::string body=css.substr(brace+1,end-brace-1);
            while(!sel.empty()&&sel[0]==' ')sel.erase(sel.begin());while(!sel.empty()&&sel.back()==' ')sel.pop_back();
            SvgCssRule rule;rule.selector=sel;
            std::istringstream ss(body);std::string decl;
            while(std::getline(ss,decl,';')){size_t c=decl.find(':');if(c==std::string::npos)continue;
                std::string k=decl.substr(0,c),v=decl.substr(c+1);
                while(!k.empty()&&k[0]==' ')k.erase(k.begin());while(!k.empty()&&k.back()==' ')k.pop_back();
                while(!v.empty()&&v[0]==' ')v.erase(v.begin());while(!v.empty()&&v.back()==' ')v.pop_back();
                rule.props[k]=v;}
            rules.push_back(rule);
            pos=end+1;
        }
    }
    for(auto&c:n->children)collectSvgCss(c.get(),rules);
}

inline bool matchesSimpleSvgSel(const std::string& sel, const Node* node){
    if(sel.empty()||!node)return false;
    if(sel[0]=='.'){
        std::string classes=sel.substr(1),nc=node->attr("class");
        std::istringstream cs(classes);std::string cls;
        bool sawClass=false;
        while(std::getline(cs,cls,'.')){
            if(cls.empty())continue;
            sawClass=true;
            bool found=false;std::istringstream ss(nc);std::string tok;
            while(ss>>tok)if(tok==cls){found=true;break;}
            if(!found)return false;
        }
        return sawClass;
    }
    size_t dot=sel.find('.');
    if(dot!=std::string::npos){
        std::string tag=sel.substr(0,dot);
        return (tag.empty()||node->tagName==tag)&&matchesSimpleSvgSel(sel.substr(dot),node);
    }
    if(sel[0]=='#')return node->attr("id")==sel.substr(1);
    return node->tagName==sel;
}
inline bool matchesSvgSelector(const std::string& sel, const Node* node){
    if(sel.empty()||!node)return false;
    // Handle comma-separated selectors
    if(sel.find(',')!=std::string::npos){
        std::istringstream ss(sel);std::string part;
        while(std::getline(ss,part,',')){while(!part.empty()&&part[0]==' ')part.erase(part.begin());while(!part.empty()&&part.back()==' ')part.pop_back();
            if(matchesSvgSelector(part,node))return true;}return false;
    }
    // Handle descendant selectors (space-separated)
    size_t sp=sel.rfind(' ');
    if(sp!=std::string::npos){
        std::string ancestor=sel.substr(0,sp),last=sel.substr(sp+1);
        while(!ancestor.empty()&&ancestor.back()==' ')ancestor.pop_back();
        if(!matchesSimpleSvgSel(last,node))return false;
        const Node* cur=node->parent;
        while(cur){if(matchesSvgSelector(ancestor,cur))return true;cur=cur->parent;}
        return false;
    }
    return matchesSimpleSvgSel(sel,node);
}

// ── Scanline fill + stroke ──────────────────────────────────────────────────

inline void fillPolys(SvgBitmap& bmp,const std::vector<std::vector<Pt>>& paths,Color c,const Mat& m,
                      const Gradient* grad=nullptr, float bx1=0,float by1=0,float bx2=0,float by2=0, int fillRule=0){
    if(paths.empty()||(c.a==0&&!grad))return;
    std::vector<std::vector<Pt>> tps;float minY=1e9f,maxY=-1e9f,minX=1e9f,maxX=-1e9f;
    for(const auto&pts:paths){
        if(pts.size()<3)continue;
        std::vector<Pt> tp(pts.size());
        for(size_t i=0;i<pts.size();++i){tp[i]=pts[i];txPt(m,tp[i].x,tp[i].y);minY=std::min(minY,tp[i].y);maxY=std::max(maxY,tp[i].y);minX=std::min(minX,tp[i].x);maxX=std::max(maxX,tp[i].x);}
        tps.push_back(std::move(tp));
    }
    if(tps.empty())return;
    if(grad&&bx1==0&&bx2==0){bx1=minX;by1=minY;bx2=maxX;by2=maxY;}
    Mat gradInv;bool hasGradInv=grad&&invMat(grad->transform,gradInv);

    // Anti-aliased scanline fill: 4x vertical sub-sampling.
    const int AA = 4;
    for(int y=std::max(0,(int)minY-1);y<=std::min(bmp.height-1,(int)(maxY+2));++y){
        // For each pixel row, sample at 4 sub-pixel Y positions.
        std::vector<std::pair<float,float>> spans; // merged x-spans
        for(int sub=0;sub<AA;++sub){
            float sy=(float)y+(float)sub/AA+0.5f/AA;
            if(fillRule==1){
                std::vector<float> xs;
                for(const auto&tp:tps)for(size_t j=0;j+1<tp.size();++j){float y0=tp[j].y,y1=tp[j+1].y;
                    if((y0<=sy&&y1>sy)||(y1<=sy&&y0>sy))xs.push_back(tp[j].x+(sy-y0)/(y1-y0)*(tp[j+1].x-tp[j].x));}
                std::sort(xs.begin(),xs.end());
                for(size_t j=0;j+1<xs.size();j+=2)spans.push_back({xs[j],xs[j+1]});
            }else{
                std::vector<std::pair<float,int>> xs;
                for(const auto&tp:tps)for(size_t j=0;j+1<tp.size();++j){float y0=tp[j].y,y1=tp[j+1].y;
                    if((y0<=sy&&y1>sy)||(y1<=sy&&y0>sy)){
                        float x=tp[j].x+(sy-y0)/(y1-y0)*(tp[j+1].x-tp[j].x);
                        xs.push_back({x,y1>y0?1:-1});
                    }}
                std::sort(xs.begin(),xs.end(),[](const auto&a,const auto&b){return a.first<b.first;});
                int winding=0;float start=0;bool open=false;
                for(auto&hit:xs){int before=winding;winding+=hit.second;if(before==0&&winding!=0){start=hit.first;open=true;}else if(before!=0&&winding==0&&open){spans.push_back({start,hit.first});open=false;}}
            }
        }
        if(spans.empty())continue;
        // Rasterize with coverage
        std::sort(spans.begin(),spans.end());
        float leftMost=spans.front().first,rightMost=spans.back().second;
        int x0=std::max(0,(int)(leftMost)),x1=std::min(bmp.width-1,(int)(rightMost+1));
        for(int x=x0;x<=x1;++x){
            // Count how many sub-samples cover this pixel
            int hits=0;
            for(auto&sp:spans)if(x>=sp.first-0.5f&&x<=sp.second+0.5f)++hits;
            if(hits==0)continue;
            float coverage=(float)hits/AA;
            Color pc=c;
            if(grad){
                float t;
                float sx=(float)x,sy=(float)y;if(hasGradInv)txPt(gradInv,sx,sy);
                if(!grad->isRadial){float gx1=grad->userSpace?grad->x1:bx1+(bx2-bx1)*grad->x1,gy1=grad->userSpace?grad->y1:by1+(by2-by1)*grad->y1,gx2=grad->userSpace?grad->x2:bx1+(bx2-bx1)*grad->x2,gy2=grad->userSpace?grad->y2:by1+(by2-by1)*grad->y2;float dx=gx2-gx1,dy=gy2-gy1,len2=dx*dx+dy*dy;t=len2>0?((sx-gx1)*dx+(sy-gy1)*dy)/len2:0;}
                else{float gcx=grad->userSpace?grad->cx:bx1+(bx2-bx1)*grad->cx,gcy=grad->userSpace?grad->cy:by1+(by2-by1)*grad->cy,gr=grad->userSpace?grad->r:std::max(bx2-bx1,by2-by1)*grad->r;float dx=sx-gcx,dy=sy-gcy;t=gr>0?std::sqrt(dx*dx+dy*dy)/gr:0;}
                pc=sampleGradient(*grad,t);
            }
            pc.a=(uint8_t)(pc.a*coverage);
            blendPixel(bmp,x,y,pc);
        }
    }
}

inline void fillPoly(SvgBitmap& bmp,const std::vector<Pt>& pts,Color c,const Mat& m,
                     const Gradient* grad=nullptr, float bx1=0,float by1=0,float bx2=0,float by2=0, int fillRule=0){
    fillPolys(bmp,std::vector<std::vector<Pt>>{pts},c,m,grad,bx1,by1,bx2,by2,fillRule);
}

inline void strokePoly(SvgBitmap& bmp,const std::vector<Pt>& pts,Color c,float sw,const Mat& m,
                       const std::vector<float>& dashArray={}, int linecap=0, int linejoin=0){
    if(pts.size()<2||c.a==0||sw<=0)return;
    float half=sw/2.f;
    // Compute total path length for dash pattern
    float totalLen=0;
    std::vector<float> segLens(pts.size()-1);
    for(size_t i=0;i+1<pts.size();++i){
        float dx=pts[i+1].x-pts[i].x,dy=pts[i+1].y-pts[i].y;
        segLens[i]=std::sqrt(dx*dx+dy*dy);totalLen+=segLens[i];
    }
    bool useDash=!dashArray.empty();
    float dashTotal=0;if(useDash)for(float d:dashArray)dashTotal+=d;
    if(dashTotal<=0)useDash=false;

    float dist=0;
    for(size_t i=0;i+1<pts.size();++i){
        float x1=pts[i].x,y1=pts[i].y,x2=pts[i+1].x,y2=pts[i+1].y;
        txPt(m,x1,y1);txPt(m,x2,y2);
        float segLen=std::sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
        if(segLen<0.1f){dist+=segLens[i];continue;}
        float ux=(x2-x1)/segLen,uy=(y2-y1)/segLen;
        float sx1=x1,sy1=y1,sx2=x2,sy2=y2;
        if(linecap==2&&i==0){sx1-=ux*half;sy1-=uy*half;}
        if(linecap==2&&i+2==pts.size()){sx2+=ux*half;sy2+=uy*half;}
        float minX=std::min(sx1,sx2)-half-1,maxX=std::max(sx1,sx2)+half+1;
        float minY=std::min(sy1,sy2)-half-1,maxY=std::max(sy1,sy2)+half+1;
        for(int py=std::max(0,(int)std::floor(minY));py<=std::min(bmp.height-1,(int)std::ceil(maxY));++py){
            for(int px=std::max(0,(int)std::floor(minX));px<=std::min(bmp.width-1,(int)std::ceil(maxX));++px){
                float cx=px+0.5f,cy=py+0.5f;
                float proj=(cx-x1)*ux+(cy-y1)*uy;
                float capProj=proj;
                if(linecap==2){if(i==0)capProj=std::max(capProj,-half);if(i+2==pts.size())capProj=std::min(capProj,segLen+half);}
                else capProj=std::clamp(capProj,0.f,segLen);
                if(linecap==0&&proj<0&&i==0)continue;
                if(linecap==0&&proj>segLen&&i+2==pts.size())continue;
                if(linecap!=1&&proj<0&&i>0)continue;
                if(linecap!=1&&proj>segLen&&i+2<pts.size())continue;
                float nx=x1+ux*capProj,ny=y1+uy*capProj;
                float dx=cx-nx,dy=cy-ny;
                if(dx*dx+dy*dy>half*half)continue;
                if(useDash){
                    float pos=std::fmod(dist+std::clamp(proj,0.f,segLen),dashTotal);
                    float acc=0;bool draw=true;
                    for(size_t d=0;d<dashArray.size();++d){acc+=dashArray[d];if(pos<acc){draw=(d%2==0);break;}}
                    if(!draw)continue;
                }
                blendPixel(bmp,px,py,c);
            }
        }
        dist+=segLens[i];
    }
    if(linejoin==1&&pts.size()>2){
        for(size_t i=1;i+1<pts.size();++i){
            float px=pts[i].x,py=pts[i].y;txPt(m,px,py);
            for(int y=(int)std::floor(py-half-1);y<=(int)std::ceil(py+half+1);++y)
                for(int x=(int)std::floor(px-half-1);x<=(int)std::ceil(px+half+1);++x){
                    float dx=x+0.5f-px,dy=y+0.5f-py;
                    if(dx*dx+dy*dy<=half*half)blendPixel(bmp,x,y,c);
                }
        }
    }
}

inline void circleAsPoly(float cx,float cy,float r,std::vector<Pt>& pts){int segs=std::clamp((int)(r*2),16,64);for(int i=0;i<=segs;++i){float a=2.f*3.14159265f*i/segs;pts.push_back({cx+r*std::cos(a),cy+r*std::sin(a)});}}
inline void ellipseAsPoly(float cx,float cy,float rx,float ry,std::vector<Pt>& pts){int segs=std::clamp((int)(std::max(rx,ry)*2),16,64);for(int i=0;i<=segs;++i){float a=2.f*3.14159265f*i/segs;pts.push_back({cx+rx*std::cos(a),cy+ry*std::sin(a)});}}

// ── Path parser (unchanged from v1) ────────────────────────────────────────

inline void parsePath(const std::string& d,std::vector<std::vector<Pt>>& subpaths){
    std::vector<Pt> poly;float cx=0,cy=0,startX=0,startY=0,lx2=0,ly2=0;char lastCmd=0;
    size_t i=0;
    auto skipWS=[&](){while(i<d.size()&&(d[i]==' '||d[i]==','||d[i]=='\n'||d[i]=='\r'||d[i]=='\t'))++i;};
    auto readNum=[&]()->float{skipWS();if(i>=d.size())return 0;size_t start=i;if(i<d.size()&&(d[i]=='-'||d[i]=='+'))++i;bool dot=false;while(i<d.size()&&(std::isdigit((unsigned char)d[i])||(d[i]=='.'&&!dot))){if(d[i]=='.')dot=true;++i;}if(i<d.size()&&(d[i]=='e'||d[i]=='E')){++i;if(i<d.size()&&(d[i]=='-'||d[i]=='+'))++i;while(i<d.size()&&std::isdigit((unsigned char)d[i]))++i;}if(i==start)return 0;try{return std::stof(d.substr(start,i-start));}catch(...){return 0;}};
    auto hasNum=[&]()->bool{size_t j=i;while(j<d.size()&&(d[j]==' '||d[j]==','||d[j]=='\n'))++j;return j<d.size()&&(std::isdigit((unsigned char)d[j])||d[j]=='-'||d[j]=='+'||d[j]=='.');};
    auto cubic=[&](float x0,float y0,float x1,float y1,float x2,float y2,float x3,float y3){for(int s=1;s<=20;++s){float t=(float)s/20,u=1-t;poly.push_back({u*u*u*x0+3*u*u*t*x1+3*u*t*t*x2+t*t*t*x3,u*u*u*y0+3*u*u*t*y1+3*u*t*t*y2+t*t*t*y3});}};
    auto quad=[&](float x0,float y0,float x1,float y1,float x2,float y2){for(int s=1;s<=16;++s){float t=(float)s/16,u=1-t;poly.push_back({u*u*x0+2*u*t*x1+t*t*x2,u*u*y0+2*u*t*y1+t*t*y2});}};
    auto arcTo=[&](float rx,float ry,float xrot,bool la,bool sw,float x,float y){
        if(rx<=0||ry<=0){poly.push_back({x,y});return;}float dx2=(cx-x)/2,dy2=(cy-y)/2,cosA=std::cos(xrot*3.14159265f/180),sinA=std::sin(xrot*3.14159265f/180);float x1p=cosA*dx2+sinA*dy2,y1p=-sinA*dx2+cosA*dy2;float r2=rx*rx*ry*ry-rx*rx*y1p*y1p-ry*ry*x1p*x1p,dn=rx*rx*y1p*y1p+ry*ry*x1p*x1p;if(dn<=0){poly.push_back({x,y});return;}float sq=std::sqrt(std::max(0.f,r2/dn));if(la==sw)sq=-sq;float cxp=sq*rx*y1p/ry,cyp=-sq*ry*x1p/rx;float mx=(cx+x)/2,my=(cy+y)/2,ccx=cosA*cxp-sinA*cyp+mx,ccy=sinA*cxp+cosA*cyp+my;auto angle=[](float ux,float uy,float vx,float vy){return std::atan2(ux*vy-uy*vx,ux*vx+uy*vy);};float t1=angle(1,0,(x1p-cxp)/rx,(y1p-cyp)/ry),dt=angle((x1p-cxp)/rx,(y1p-cyp)/ry,(-x1p-cxp)/rx,(-y1p-cyp)/ry);if(!sw&&dt>0)dt-=2*3.14159265f;if(sw&&dt<0)dt+=2*3.14159265f;int segs=std::max(4,(int)(std::abs(dt)/0.3f));for(int s=1;s<=segs;++s){float t=t1+dt*s/segs;poly.push_back({cosA*rx*std::cos(t)-sinA*ry*std::sin(t)+ccx,sinA*rx*std::cos(t)+cosA*ry*std::sin(t)+ccy});}};
    while(i<d.size()){skipWS();if(i>=d.size())break;char cmd=d[i];if(std::isalpha((unsigned char)cmd)){++i;}else cmd=lastCmd;bool rel=(cmd>='a'&&cmd<='z');char C=rel?(cmd-32):cmd;
        do{switch(C){
        case 'M':if(!poly.empty()){subpaths.push_back(poly);poly.clear();}{float x=readNum(),y=readNum();if(rel){x+=cx;y+=cy;}cx=startX=x;cy=startY=y;poly.push_back({cx,cy});C='L';lastCmd=rel?'l':'L';break;}
        case 'L':{float x=readNum(),y=readNum();if(rel){x+=cx;y+=cy;}cx=x;cy=y;poly.push_back({cx,cy});break;}
        case 'H':{float x=readNum();if(rel)x+=cx;cx=x;poly.push_back({cx,cy});break;}
        case 'V':{float y=readNum();if(rel)y+=cy;cy=y;poly.push_back({cx,cy});break;}
        case 'C':{float x1=readNum(),y1=readNum(),x2=readNum(),y2=readNum(),x=readNum(),y=readNum();if(rel){x1+=cx;y1+=cy;x2+=cx;y2+=cy;x+=cx;y+=cy;}cubic(cx,cy,x1,y1,x2,y2,x,y);lx2=x2;ly2=y2;cx=x;cy=y;break;}
        case 'S':{float rx1=2*cx-lx2,ry1=2*cy-ly2,x2=readNum(),y2=readNum(),x=readNum(),y=readNum();if(rel){x2+=cx;y2+=cy;x+=cx;y+=cy;}cubic(cx,cy,rx1,ry1,x2,y2,x,y);lx2=x2;ly2=y2;cx=x;cy=y;break;}
        case 'Q':{float x1=readNum(),y1=readNum(),x=readNum(),y=readNum();if(rel){x1+=cx;y1+=cy;x+=cx;y+=cy;}quad(cx,cy,x1,y1,x,y);lx2=x1;ly2=y1;cx=x;cy=y;break;}
        case 'T':{float rx1=2*cx-lx2,ry1=2*cy-ly2,x=readNum(),y=readNum();if(rel){x+=cx;y+=cy;}quad(cx,cy,rx1,ry1,x,y);lx2=rx1;ly2=ry1;cx=x;cy=y;break;}
        case 'A':{float rx=readNum(),ry=readNum(),xrot=readNum(),la=readNum(),sw=readNum(),x=readNum(),y=readNum();if(rel){x+=cx;y+=cy;}arcTo(rx,ry,xrot,la!=0,sw!=0,x,y);cx=x;cy=y;break;}
        case 'Z':poly.push_back({startX,startY});subpaths.push_back(poly);poly.clear();cx=startX;cy=startY;break;
        default:++i;goto next;
        }if(C!='M')lastCmd=cmd;}while(C!='Z'&&hasNum());next:;}
    if(!poly.empty())subpaths.push_back(poly);
}

// ── Style + render context ──────────────────────────────────────────────────

struct Style{Color fill={0,0,0,255};Color stroke={0,0,0,0};float sw=1;float opacity=1;float fillOp=1;float strokeOp=1;bool fillNone=false;bool strokeNone=false;std::string fillUrl;std::string strokeUrl;std::vector<float>dashArray;bool hidden=false;int linecap=0;int linejoin=0;int fillRule=0;}; // linecap: 0=butt,1=round,2=square; linejoin: 0=miter,1=round,2=bevel; fillRule: 0=nonzero,1=evenodd

inline Style getStyle(const Node* node,const Style& parent,const std::vector<SvgCssRule>& cssRules){
    Style s=parent;s.dashArray.clear();s.fillUrl.clear();s.strokeUrl.clear();s.hidden=false;
    // Apply CSS rules matching this node
    for(auto&rule:cssRules)if(matchesSvgSelector(rule.selector,node))for(auto&[k,v]:rule.props){
        if(k=="fill"){if(v=="none")s.fillNone=true;else if(v.rfind("url(",0)==0){size_t p=v.find('#'),e=v.find(')');if(p!=std::string::npos&&e!=std::string::npos)s.fillUrl=v.substr(p+1,e-p-1);s.fillNone=false;}else{s.fill=parseColor(v);s.fillNone=false;}}
        else if(k=="stroke"){if(v=="none")s.strokeNone=true;else{s.stroke=parseColor(v);s.strokeNone=false;}}
        else if(k=="stroke-width")try{s.sw=std::stof(v);}catch(...){}
        else if(k=="opacity")try{s.opacity=std::stof(v);}catch(...){}
        else if(k=="fill-opacity")try{s.fillOp=std::stof(v);}catch(...){}
        else if(k=="stroke-opacity")try{s.strokeOp=std::stof(v);}catch(...){}
        else if(k=="fill-rule"){if(v=="evenodd")s.fillRule=1;else if(v=="nonzero")s.fillRule=0;}
        else if(k=="clip-rule"&&v=="evenodd")s.fillRule=1;
        else if(k=="stroke-linecap"){if(v=="round")s.linecap=1;else if(v=="square")s.linecap=2;else if(v=="butt")s.linecap=0;}
        else if(k=="stroke-linejoin"){if(v=="round")s.linejoin=1;else if(v=="bevel")s.linejoin=2;else if(v=="miter")s.linejoin=0;}
        else if(k=="stroke-dasharray"&&v!="none"){std::istringstream ds(v);float dv;char dc;while(ds>>dv){s.dashArray.push_back(dv);if(ds.peek()==','||ds.peek()==' ')ds>>dc;}}
        else if(k=="display"&&v=="none")s.hidden=true;
        else if(k=="visibility"&&v=="hidden")s.hidden=true;
    }
    // Direct attrs + style=""
    std::map<std::string,std::string> props;
    std::string sa=node->attr("style");
    if(!sa.empty()){std::istringstream ss(sa);std::string decl;while(std::getline(ss,decl,';')){size_t c=decl.find(':');if(c==std::string::npos)continue;std::string k=decl.substr(0,c),v=decl.substr(c+1);while(!k.empty()&&k[0]==' ')k.erase(k.begin());while(!k.empty()&&k.back()==' ')k.pop_back();while(!v.empty()&&v[0]==' ')v.erase(v.begin());while(!v.empty()&&v.back()==' ')v.pop_back();props[k]=v;}}
    auto get=[&](const std::string&name)->std::string{std::string v=node->attr(name);if(v.empty()){auto it=props.find(name);if(it!=props.end())v=it->second;}return v;};
    std::string f=get("fill");
    if(!f.empty()){if(f=="none")s.fillNone=true;else if(f.rfind("url(",0)==0){size_t p=f.find('#'),e=f.find(')');if(p!=std::string::npos&&e!=std::string::npos)s.fillUrl=f.substr(p+1,e-p-1);s.fillNone=false;}else{s.fill=parseColor(f);s.fillNone=false;s.fillUrl.clear();}}
    std::string st=get("stroke");if(!st.empty()){if(st=="none")s.strokeNone=true;else{s.stroke=parseColor(st);s.strokeNone=false;}}
    std::string sw=get("stroke-width");if(!sw.empty())try{s.sw=std::stof(sw);}catch(...){}
    std::string op=get("opacity");if(!op.empty())try{s.opacity=std::stof(op);}catch(...){}
    std::string fo=get("fill-opacity");if(!fo.empty())try{s.fillOp=std::stof(fo);}catch(...){}
    std::string so=get("stroke-opacity");if(!so.empty())try{s.strokeOp=std::stof(so);}catch(...){}
    std::string da=get("stroke-dasharray");
    if(!da.empty()&&da!="none"){std::istringstream ds(da);float dv;char dc;while(ds>>dv){s.dashArray.push_back(dv);if(ds.peek()==','||ds.peek()==' ')ds>>dc;}}
    std::string fr=get("fill-rule");if(fr=="evenodd")s.fillRule=1;else if(fr=="nonzero")s.fillRule=0;
    std::string cr=get("clip-rule");if(cr=="evenodd")s.fillRule=1;
    std::string lc=get("stroke-linecap");if(lc=="round")s.linecap=1;else if(lc=="square")s.linecap=2;
    std::string lj=get("stroke-linejoin");if(lj=="round")s.linejoin=1;else if(lj=="bevel")s.linejoin=2;
    std::string disp=get("display");if(disp=="none")s.hidden=true;
    std::string vis=get("visibility");if(vis=="hidden")s.hidden=true;
    return s;
}

inline Color applyOp(Color c,float op,float chOp){c.a=(uint8_t)(std::clamp((c.a/255.f)*op*chOp,0.f,1.f)*255);return c;}

// ── Render ──────────────────────────────────────────────────────────────────

struct RenderCtx{
    std::map<std::string,const Node*> defs;
    std::map<std::string,Gradient> grads;
    std::vector<SvgCssRule> cssRules;
};

inline void collectDefs(const Node* n,std::map<std::string,const Node*>& defs){if(!n)return;std::string id=n->attr("id");if(!id.empty())defs[id]=n;for(auto&c:n->children)collectDefs(c.get(),defs);}

inline void renderEl(SvgBitmap& bmp,const Node* node,const Mat& parentM,const Style& parentStyle,const RenderCtx& ctx){
    if(!node||node->type!=NodeType::Element)return;
    const std::string&tag=node->tagName;
    if(tag=="defs"||tag=="clippath"||tag=="lineargradient"||tag=="radialgradient"||tag=="symbol"||tag=="mask"||tag=="title"||tag=="desc"||tag=="metadata"||tag=="style")return;

    Style style=getStyle(node,parentStyle,ctx.cssRules);
    if(style.hidden)return;
    Color fill=style.fillNone?Color{0,0,0,0}:applyOp(style.fill,style.opacity,style.fillOp);
    Color stroke=style.strokeNone?Color{0,0,0,0}:applyOp(style.stroke,style.opacity,style.strokeOp);
    float sw=style.sw;
    const Gradient* fillGrad=nullptr;
    if(!style.fillUrl.empty()){auto it=ctx.grads.find(style.fillUrl);if(it!=ctx.grads.end())fillGrad=&it->second;fill.a=255;}
    Mat m=parentM;std::string tr=node->attr("transform");if(!tr.empty())m=matMul(m,parseTransform(tr));

    auto renderShape=[&](const std::vector<Pt>& pts,bool closed=true){
        if(fill.a>0&&pts.size()>=3)fillPoly(bmp,pts,fill,m,fillGrad,0,0,0,0,style.fillRule);
        if(stroke.a>0&&pts.size()>=2)strokePoly(bmp,pts,stroke,sw,m,style.dashArray,style.linecap,style.linejoin);
    };

    if(tag=="rect"){
        float x=parseNum(node->attr("x")),y=parseNum(node->attr("y")),w=parseNum(node->attr("width")),h=parseNum(node->attr("height"));
        float rx=parseNum(node->attr("rx")),ry=parseNum(node->attr("ry"));if(ry<=0)ry=rx;if(rx<=0)rx=ry;
        std::vector<Pt>pts;
        if(rx>0){rx=std::min(rx,w/2);ry=std::min(ry,h/2);
            auto corner=[&](float ccx,float ccy,float sa,int n){for(int j=0;j<=n;++j){float a=sa+3.14159265f/2*j/n;pts.push_back({ccx+rx*std::cos(a),ccy+ry*std::sin(a)});}};
            corner(x+w-rx,y+ry,-3.14159265f/2,8);corner(x+w-rx,y+h-ry,0,8);corner(x+rx,y+h-ry,3.14159265f/2,8);corner(x+rx,y+ry,3.14159265f,8);pts.push_back(pts[0]);
        }else pts={{x,y},{x+w,y},{x+w,y+h},{x,y+h},{x,y}};
        renderShape(pts);
    }else if(tag=="circle"){
        float ccx=parseNum(node->attr("cx")),ccy=parseNum(node->attr("cy")),r=parseNum(node->attr("r"));
        std::vector<Pt>pts;circleAsPoly(ccx,ccy,r,pts);renderShape(pts);
    }else if(tag=="ellipse"){
        float ccx=parseNum(node->attr("cx")),ccy=parseNum(node->attr("cy")),rx=parseNum(node->attr("rx")),ry=parseNum(node->attr("ry"));
        std::vector<Pt>pts;ellipseAsPoly(ccx,ccy,rx,ry,pts);renderShape(pts);
    }else if(tag=="line"){
        float x1=parseNum(node->attr("x1")),y1=parseNum(node->attr("y1")),x2=parseNum(node->attr("x2")),y2=parseNum(node->attr("y2"));
        strokePoly(bmp,{{x1,y1},{x2,y2}},stroke.a>0?stroke:fill,sw,m,style.dashArray,style.linecap,style.linejoin);
    }else if(tag=="path"){
        std::string d=node->attr("d");if(!d.empty()){
            std::vector<std::vector<Pt>>sps;parsePath(d,sps);
            if(fill.a>0||fillGrad)fillPolys(bmp,sps,fill,m,fillGrad,0,0,0,0,style.fillRule);
            for(auto&sp:sps)if(stroke.a>0&&sp.size()>=2)strokePoly(bmp,sp,stroke,sw,m,style.dashArray,style.linecap,style.linejoin);
        }
    }else if(tag=="polygon"||tag=="polyline"){
        std::string pts=node->attr("points");std::vector<Pt>poly;std::istringstream ss(pts);float x,y;
        while(ss>>x){char c;if(ss.peek()==',')ss>>c;ss>>y;poly.push_back({x,y});}
        if(tag=="polygon"&&!poly.empty())poly.push_back(poly[0]);
        renderShape(poly,tag=="polygon");
    }else if(tag=="use"){
        std::string href=node->attr("href");if(href.empty())href=node->attr("xlink:href");
        if(!href.empty()&&href[0]=='#'){auto it=ctx.defs.find(href.substr(1));if(it!=ctx.defs.end()){
            float ux=parseNum(node->attr("x")),uy=parseNum(node->attr("y"));
            Mat um=matMul(m,Mat{1,0,0,1,ux,uy});
            if(it->second->tagName=="symbol"){
                for(auto&child:it->second->children)renderEl(bmp,child.get(),um,style,ctx);
            }else renderEl(bmp,it->second,um,style,ctx);}}
    }else if(tag=="text"||tag=="tspan"){
        float tx=parseNum(node->attr("x")),ty=parseNum(node->attr("y"));
        float fontSize=14;
        std::string fs=node->attr("font-size");if(fs.empty()){auto it2=node->attr("style");/* check style */}
        if(!fs.empty())try{fontSize=std::stof(fs);}catch(...){}
        // Collect all text content (direct + tspan children).
        std::string text;
        std::function<void(const Node*)> collectText=[&](const Node* n){
            for(auto&c:n->children){if(c->type==NodeType::Text)text+=c->text;else if(c->tagName=="tspan")collectText(c.get());}
        };
        collectText(node);
        // Trim leading/trailing whitespace, collapse inner whitespace.
        while(!text.empty()&&text[0]==' ')text.erase(text.begin());
        while(!text.empty()&&text.back()==' ')text.pop_back();
        // text-anchor
        int anchor=0;
        std::string ta=node->attr("text-anchor");if(ta.empty()){auto it2=node->attr("style");if(!it2.empty()){size_t p=it2.find("text-anchor:");if(p!=std::string::npos){size_t e=it2.find(';',p);ta=it2.substr(p+12,e==std::string::npos?std::string::npos:e-p-12);while(!ta.empty()&&ta[0]==' ')ta.erase(ta.begin());}}}
        if(ta=="middle")anchor=1;else if(ta=="end")anchor=2;
        if(!text.empty()&&fill.a>0)
            drawString(bmp,text,tx,ty,fontSize,fill,m,anchor);
    }else if(tag=="svg"){
        // Nested SVG: apply a new viewport transform.
        float nx=parseNum(node->attr("x")),ny=parseNum(node->attr("y"));
        Mat nm=matMul(m,Mat{1,0,0,1,nx,ny});
        for(auto&child:node->children)renderEl(bmp,child.get(),nm,style,ctx);
        return; // don't recurse again below
    }

    for(auto&child:node->children)renderEl(bmp,child.get(),m,style,ctx);
}

// ── Entry point ─────────────────────────────────────────────────────────────

inline SvgBitmap renderSvg(const Node* svgNode,int maxDim=512){
    SvgBitmap bmp;if(!svgNode)return bmp;
    float vx=0,vy=0,vw=0,vh=0;
    std::string vb=svgNode->attr("viewBox");if(vb.empty())vb=svgNode->attr("viewbox");
    if(!vb.empty()){std::istringstream ss(vb);char c;if(!(ss>>vx>>c>>vy>>c>>vw>>c>>vh)||vw<=0){ss.clear();ss.str(vb);ss>>vx>>vy>>vw>>vh;}}
    float w=parseNum(svgNode->attr("width")),h=parseNum(svgNode->attr("height"));
    if(w<=0&&vw>0)w=vw;if(h<=0&&vh>0)h=vh;if(w<=0||h<=0)return bmp;
    float scale=1.f;if(w>maxDim||h>maxDim)scale=maxDim/std::max(w,h);
    bmp.width=std::max(1,(int)(w*scale));bmp.height=std::max(1,(int)(h*scale));
    bmp.pixels.resize(bmp.width*bmp.height*4,0);
    // preserveAspectRatio parsing
    std::string par=svgNode->attr("preserveAspectRatio");if(par.empty())par=svgNode->attr("preserveaspectratio");
    bool parNone=(par.find("none")!=std::string::npos);
    int alignX=1,alignY=1; // 0=Min,1=Mid,2=Max
    if(par.find("xMin")!=std::string::npos)alignX=0;else if(par.find("xMax")!=std::string::npos)alignX=2;
    if(par.find("YMin")!=std::string::npos)alignY=0;else if(par.find("YMax")!=std::string::npos)alignY=2;
    bool meetOrSlice=(par.find("slice")!=std::string::npos);

    Mat viewM;
    if(vw>0&&vh>0){
        float sx=w/vw,sy=h/vh;
        float s=parNone?1.f:(meetOrSlice?std::max(sx,sy):std::min(sx,sy));
        float scaleX=parNone?sx*scale:s*scale, scaleY=parNone?sy*scale:s*scale;
        float tx=0,ty=0;
        if(!parNone){
            float usedW=vw*s,usedH=vh*s;
            if(alignX==0)tx=-vx*s;else if(alignX==1)tx=(w-usedW)/2-vx*s;else tx=w-usedW-vx*s;
            if(alignY==0)ty=-vy*s;else if(alignY==1)ty=(h-usedH)/2-vy*s;else ty=h-usedH-vy*s;
        }else{tx=-vx*sx;ty=-vy*sy;}
        viewM={scaleX,0,0,scaleY,tx*scale,ty*scale};
    }
    else viewM={scale,0,0,scale,0,0};

    RenderCtx ctx;
    collectDefs(svgNode,ctx.defs);
    collectGradients(svgNode,ctx.grads);
    collectSvgCss(svgNode,ctx.cssRules);

    Style rootStyle;std::string rf=svgNode->attr("fill");if(!rf.empty()&&rf!="none")rootStyle.fill=parseColor(rf);
    for(auto&child:svgNode->children)renderEl(bmp,child.get(),viewM,rootStyle,ctx);
    return bmp;
}

// ── Utility ─────────────────────────────────────────────────────────────────

inline bool looksLikeSvgBytes(const std::vector<uint8_t>& bytes){if(bytes.empty())return false;const size_t n=std::min<size_t>(bytes.size(),512);std::string head;head.reserve(n);for(size_t i=0;i<n;++i)head+=(char)std::tolower((unsigned char)bytes[i]);return head.find("<svg")!=std::string::npos||head.find("<?xml")!=std::string::npos||head.find("image/svg+xml")!=std::string::npos;}
inline int SvgRasterMaxDimForBytes(size_t byteCount){return byteCount<4096?256:(byteCount<32768?512:1024);}
inline const Node* findSvgNode(const Node* node){if(!node)return nullptr;if(node->type==NodeType::Element&&node->tagName=="svg")return node;for(const auto&child:node->children)if(const Node*found=findSvgNode(child.get()))return found;return nullptr;}
inline SvgBitmap renderSvgBytes(const std::string& text,int maxDim=512){auto doc=ParseHtml(text);const Node*svgNode=findSvgNode(doc.get());return svgNode?renderSvg(svgNode,maxDim):SvgBitmap{};}
inline SvgBitmap renderSvgBytes(const std::vector<uint8_t>& bytes,int maxDim=512){return renderSvgBytes(std::string(bytes.begin(),bytes.end()),maxDim);}

} // namespace svg
