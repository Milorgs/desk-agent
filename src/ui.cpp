#include "ui.hpp"
#include "platform.hpp"
#include <cstring>

bool ui_color(){return terminal_supports_color();}

namespace {
const char *sgr(Tone tone){
    switch(tone){
    case Tone::Dim:return "\033[38;5;247m"; // grey, not SGR 2: faint text vanishes on translucent backgrounds
    case Tone::Bold:return "\033[1m";
    case Tone::Accent:return "\033[38;5;111m";
    case Tone::Success:return "\033[38;5;114m";
    case Tone::Warning:return "\033[38;5;179m";
    case Tone::Error:return "\033[38;5;174m";
    case Tone::Rule:return "\033[38;5;240m";
    case Tone::Plain:break;
    }
    return "";
}
// Decode one UTF-8 code point; returns the byte length, 0 on a bad sequence.
size_t decode(std::string_view text,uint32_t &code){
    if(text.empty())return 0;
    auto first=static_cast<unsigned char>(text[0]);
    size_t width=first<0x80?1:first<0xe0?2:first<0xf0?3:first<0xf4?4:0;
    if(!width||width>text.size())return 0;
    // A lead byte carries 8-width payload bits: 7 for ASCII, 5 for a 3-byte lead.
    code=first&((1u<<(8-width))-1);
    for(size_t i=1;i<width;i++){
        auto byte=static_cast<unsigned char>(text[i]);
        if((byte&0xc0)!=0x80)return 0;
        code=(code<<6)|(byte&0x3f);
    }
    return width;
}
// East Asian wide and fullwidth ranges take two cells; combining marks take none.
int cell_width(uint32_t code){
    if(code<32||code==127)return 0;
    if((code>=0x0300&&code<=0x036f)||(code>=0x200b&&code<=0x200f)||(code>=0xfe00&&code<=0xfe0f))return 0;
    if((code>=0x1100&&code<=0x115f)||(code>=0x2e80&&code<=0xa4cf)||
       (code>=0xac00&&code<=0xd7a3)||(code>=0xf900&&code<=0xfaff)||
       (code>=0xfe30&&code<=0xfe6f)||(code>=0xff00&&code<=0xff60)||
       (code>=0xffe0&&code<=0xffe6)||(code>=0x1f300&&code<=0x1f64f)||
       (code>=0x1f900&&code<=0x1f9ff)||(code>=0x20000&&code<=0x3fffd))return 2;
    return 1;
}
}

std::string ui_paint(Tone tone,std::string_view text,bool bold){
    const char *code=sgr(tone);
    if(!ui_color())return std::string(text);
    std::string result;
    if(bold)result+="\033[1m";
    result+=code;
    result.append(text);
    result+="\033[0m";
    return result;
}

int ui_width(std::string_view text){
    int cells=0;
    while(!text.empty()){
        uint32_t code=0;
        size_t size=decode(text,code);
        if(!size){text.remove_prefix(1);cells++;continue;}
        cells+=cell_width(code);
        text.remove_prefix(size);
    }
    return cells;
}

std::string ui_clip(std::string_view text,int width){
    if(width<=0)return std::string(text);
    if(ui_width(text)<=width)return std::string(text);
    std::string result;
    int cells=0;
    while(!text.empty()){
        uint32_t code=0;
        size_t size=decode(text,code);
        if(!size)break;
        int advance=cell_width(code);
        if(cells+advance>width-1)break;
        result.append(text.substr(0,size));
        cells+=advance;
        text.remove_prefix(size);
    }
    result+="…";
    return result;
}

std::string ui_fit(std::string_view text,int margin){
    const int columns=terminal_columns();
    if(columns<=0)return std::string(text);
    return ui_clip(text,std::max(24,columns-margin));
}

std::vector<std::string> ui_wrap(std::string_view text,int width){
    std::vector<std::string> lines;
    if(width<=0){lines.emplace_back(text);return lines;}
    std::string current;
    int cells=0;
    // Spaces stay verbatim so indented bodies survive ui_box untouched; only
    // spaces that would open a wrapped line are dropped.
    bool wrapped=false;
    auto flush=[&]{
        lines.push_back(current);
        current.clear();
        cells=0;
        wrapped=true;
    };
    size_t index=0;
    while(index<text.size()){
        // Take one code point at a time so wrapping never splits a character.
        uint32_t code=0;
        size_t size=decode(text.substr(index),code);
        if(!size)size=1;
        if(code==' '||code=='\t'){
            if(wrapped&&cells==0){index+=size;continue;}
            if(cells+1>width){flush();index+=size;continue;}
            current+=' ';
            cells++;
            index+=size;
            continue;
        }
        int advance=cell_width(code);
        if(cells+advance>width)flush();
        current.append(text.substr(index,size));
        cells+=advance;
        index+=size;
    }
    if(!current.empty()||lines.empty())lines.push_back(current);
    return lines;
}

std::string ui_box(std::string_view title,std::string_view body,int width){
    if(width<10)return std::string(body);
    const int inner=width-4;
    std::string result;
    std::string top=title.empty()?std::string("╭"):std::string("╭─ ");
    if(!title.empty()){top+=ui_clip(title,inner-3);top+=' ';}
    while(ui_width(top)<width-1)top+="─";
    top+="╮";
    result+=ui_paint(Tone::Rule,top);
    result+='\n';
    std::string subject(body);
    size_t start=0;
    while(true){
        auto end=subject.find('\n',start);
        auto line=std::string_view(subject).substr(start,end==std::string::npos?std::string::npos:end-start);
        for(const auto &piece:ui_wrap(line,inner)){
            std::string row="│ ";
            row+=piece;
            int pad=inner-ui_width(piece);
            if(pad>0)row.append(static_cast<size_t>(pad),' ');
            row+=" │";
            result+=row;
            result+='\n';
        }
        if(end==std::string::npos)break;
        start=end+1;
    }
    std::string bottom="╰";
    while(ui_width(bottom)<width-1)bottom+="─";
    bottom+="╯";
    result+=ui_paint(Tone::Rule,bottom);
    return result;
}

const char *ui_bullet(){return "⏺";}
const char *ui_result(){return "⎿";}
const char *ui_prompt(){return "❯";}

namespace {
// Braille spinner keeps the same visual weight at any terminal font.
const char *const spinner[]={"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
}
const char *ui_spinner(size_t frame){return spinner[frame%ui_spinner_frames()];}
size_t ui_spinner_frames(){return sizeof spinner/sizeof spinner[0];}
