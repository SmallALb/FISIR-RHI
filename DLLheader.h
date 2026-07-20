#pragma once

#ifdef _WIN32
#define EXPORTDLL __declspec(dllexport)
#else
#define EXPORTDLL
#endif // _WIN32
