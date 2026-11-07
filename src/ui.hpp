#ifndef DESK_UI_HPP
#define DESK_UI_HPP
#include <string>
#include <string_view>
#include <vector>
// Shared visual language for every page and chat renderer.
// Colour is dropped automatically when the terminal cannot show it.
enum class Tone { Plain, Dim, Bold, Accent, Success, Warning, Error, Rule };
bool ui_color();
std::string ui_paint(Tone tone,std::string_view text,bool bold=false);
// Display width in terminal cells; UTF-8 aware without depending on the locale.
int ui_width(std::string_view text);
std::string ui_clip(std::string_view text,int width);
// Clip to the terminal width minus a margin; leaves the text untouched when the
// width is unknown (pipes, some test harnesses), so nothing is lost there.
std::string ui_fit(std::string_view text,int margin);
std::vector<std::string> ui_wrap(std::string_view text,int width);
std::string ui_box(std::string_view title,std::string_view body,int width);
// Glyphs that give the interface its shape.
const char *ui_bullet();
const char *ui_result();
const char *ui_prompt();
const char *ui_spinner(size_t frame);
size_t ui_spinner_frames();
#endif
