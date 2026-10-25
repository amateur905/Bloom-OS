#pragma once

#define E_NO_VOLUMES      100
#define E_FOLDER_READ     102
#define E_FOLDER_LARGE    103
#define E_NAME_LONG       104
#define E_PATH_LONG       105
#define E_FILE_OPEN       110
#define E_FILE_READ       111
#define E_FILE_TRUNCATED  112
#define E_FILE_BINARY     113
#define E_FILE_EMPTY      114
#define E_VIEW_LINES      115
#define E_VIEW_RANGE      116
#define E_AUDIO_RATE      120
#define E_AUDIO_FORMAT    121
#define E_AUDIO_DEVICE    122
#define E_AUDIO_MEMORY    123
#define E_AUDIO_READ      124
#define E_AUDIO_NOTWAV    125
#define E_INTERNAL_SEL    130
#define E_SCREEN_SMALL    131
#define E_TEXT_NOTFOUND   140
#define E_INFO_FAILED     141

const char *ErrorText(int Code);
int ErrorIsWarning(int Code);
void ErrorFormat(int Code, const char *Detail, char *Out, int Max);
void ErrorLog(const char *Source, int Code, const char *Detail);
