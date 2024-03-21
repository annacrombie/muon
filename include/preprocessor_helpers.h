#ifndef MUON_PREPROCESSOR_HELPERS_H
#define MUON_PREPROCESSOR_HELPERS_H

#define STRINGIZE(x) STRINGIZE2(x)
#define STRINGIZE2(x) #x
#define LINE_STRING STRINGIZE(__LINE__)

#define CONCAT(first, second) CONCAT_SIMPLE(first, second)
#define CONCAT_SIMPLE(first, second) first##second

#endif
