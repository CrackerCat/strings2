// strings.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "string_parser.h"
#include "windows.h"
#include <sys/types.h>
#include "dirent.h"
#include <errno.h>
#include <vector>
#include <string>
#include <iostream>
#include "Shlwapi.h"
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include "process_strings.h"

using namespace std;


BOOL is_win64()
{
	#if defined(_WIN64)
		return TRUE;  // 64-bit programs run only on Win64
	#elif defined(_WIN32)
		// 32-bit programs run on both 32-bit and 64-bit Windows
		// so must sniff
		BOOL f64 = FALSE;
		return IsWow64Process(GetCurrentProcess(), &f64) && f64;
	#else
		return FALSE; // Win64 does not support Win16
	#endif
}

bool is_elevated(HANDLE h_Process)
{
	HANDLE h_Token;
	TOKEN_ELEVATION t_TokenElevation;
    TOKEN_ELEVATION_TYPE e_ElevationType;
	DWORD dw_TokenLength;
	
	if( OpenProcessToken(h_Process, TOKEN_READ | TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES , &h_Token) )
	{
		if(GetTokenInformation(h_Token,TokenElevation,&t_TokenElevation,sizeof(t_TokenElevation),&dw_TokenLength))
		{
			if(t_TokenElevation.TokenIsElevated != 0)
			{
				if(GetTokenInformation(h_Token,TokenElevationType,&e_ElevationType,sizeof(e_ElevationType),&dw_TokenLength))
				{
					if(e_ElevationType == TokenElevationTypeFull || e_ElevationType == TokenElevationTypeDefault)
					{
						return true;
					}
				}
			}
		}
	}

    return false;
}

void process_folder( char* dir_name, WCHAR* filter, bool recursively, string_parser* parser )
{
	DIR *dir;
	struct dirent *ent;
	dir = opendir (dir_name);
	if (dir != NULL)
	{
		/* print all the files and directories within directory */
		while ((ent = readdir (dir)) != NULL) {
			// Convert the path to wchar format
			wchar_t* result = new wchar_t[ent->d_namlen + 1];

			if( result != NULL )
			{
				for( int i = 0; i < ent->d_namlen; i++ )
					result[i] = ent->d_name[i];
				result[ent->d_namlen] = 0;

				if( (ent->d_type & DT_DIR) )
				{
					// Process this subdirectory if recursive flag is on
					if( recursively && wcscmp(result, L".") != 0 && wcscmp(result, L"..") != 0  )
					{
						// Build the directory path
						char* directory = new char[MAX_PATH+1];
						sprintf_s(directory, MAX_PATH+1, "%s/%s", dir_name, ent->d_name);

						process_folder( directory, filter, recursively, parser );

						// Cleanup
						delete[] directory;
					}
				}else{
					// Check if this filename is a match to the specified pattern
					if( PathMatchSpec( result, filter ) )
					{
						// Process this file
						int length = wcslen(result) + strlen(dir_name) + 1;
						char* filename = new char[length + 1];
						filename[length] = 0;
						sprintf_s( filename, length+1, "%s/%S", dir_name, result );

						// Processes the specified file for strings
						FILE* fh = fopen( filename, "rb" );
						if( fh != NULL )
						{
							parser->parse_stream(fh, filename);
							fclose(fh);
						}else{
							// Error
							fprintf(stderr, "Error opening file %s: %s.\n", filename, strerror(errno));
						}

						delete[] filename;
					}
				}
				delete[] result;
			}
			else
			{
				fprintf(stderr, "Failed to allocate memory block of size %i for filename: %s.\n", ent->d_namlen + 1, strerror(errno));
			}
		}
		closedir (dir);
	}else{
		fprintf(stderr, "Unable to open directory %s: %s.\n", dir_name, strerror(errno));
	}
}


bool get_privileges(HANDLE h_Process)
{
	HANDLE h_Token;
	DWORD dw_TokenLength;
	if( OpenProcessToken(h_Process, TOKEN_READ | TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES , &h_Token) )
	{
		// Read the old token privileges
		TOKEN_PRIVILEGES* privilages = new TOKEN_PRIVILEGES[100];
		if( GetTokenInformation(h_Token, TokenPrivileges, privilages,sizeof(TOKEN_PRIVILEGES)*100,&dw_TokenLength) )
		{
			// Enable all privileges
			for( int i = 0; i < privilages->PrivilegeCount; i++ )
			{
				privilages->Privileges[i].Attributes = SE_PRIVILEGE_ENABLED;
			}
			
			// Adjust the privilges
			if(AdjustTokenPrivileges( h_Token, false, privilages, sizeof(TOKEN_PRIVILEGES)*100, NULL, NULL  ))
			{
				delete[] privilages;
				return true;
			}
		}
		delete[] privilages;
	}
	return false;
}

int _tmain(int argc, _TCHAR* argv[])
{
	//char buf[1000000];
	//setvbuf(stdout, buf, _IOFBF, sizeof(buf));

	// Process the flags	
	WCHAR* filter = NULL;
	bool flag_help = false;
	bool flag_filename = false;
	bool flag_filepath = false;
	bool flag_utf8_only = false;
	bool flag_wide_string_only = false;
	bool piped_input = !_isatty( _fileno( stdin ) );
	bool flag_dump_pid = false;
	bool flag_dump_system = false;
	bool flag_recursive = false;
	bool flag_all_strings = false;
	bool flag_strings_interesting = true;
	bool flag_strings_not_interesting = false;
	bool flag_print_string_type = false;
	bool flag_print_json = false;
	WCHAR* flag_json_file = NULL;
	int minCharacters = 4;

	if( argc <= 1 && !piped_input )
		flag_help = true;
	for( int i = 1; i < argc; i++ )
	{
		if (lstrcmp(argv[i], L"--help") == 0 || lstrcmp(argv[i], L"-help") == 0 || lstrcmp(argv[i], L"-h") == 0 || lstrcmp(argv[i], L"--h") == 0)
			flag_help = true;
		else if (lstrcmp(argv[i], L"-f") == 0)
			flag_filename = true;
		else if (lstrcmp(argv[i], L"-F") == 0)
			flag_filepath = true;
		else if (lstrcmp(argv[i], L"-r") == 0)
			flag_recursive = true;
		else if (lstrcmp(argv[i], L"-t") == 0)
			flag_print_string_type = true;
		else if (lstrcmp(argv[i], L"-json") == 0)
		{
			flag_print_json = true;

			if (i + 1 < argc)
			{
				flag_json_file = argv[i+1];
			}
			else {
				fprintf(stderr, "Failed to parse -json argument. It must be followed by a file:\n\teg. 'strings2 *.exe -json result.json'\n");
				exit(0);
			}
		}
		else if (lstrcmp(argv[i], L"-a") == 0)
		{
			// Both interesting and not interesting
			flag_strings_interesting = true;
			flag_strings_not_interesting = true;
		}
		else if (lstrcmp(argv[i], L"-ni") == 0)
		{
			// Only not interesting
			flag_strings_interesting = false;
			flag_strings_not_interesting = true;
		}
		else if(lstrcmp(argv[i], L"-u") == 0 || lstrcmp(argv[i],L"-utf8") == 0 )
			flag_utf8_only = true;
		else if(lstrcmp(argv[i], L"-w") == 0 || lstrcmp(argv[i],L"-wide") == 0 )
			flag_wide_string_only = true;
		else if( lstrcmp(argv[i],L"-pid") == 0 )
			flag_dump_pid = true;
		else if( lstrcmp(argv[i],L"-system") == 0 )
			flag_dump_system = true;
		else if( lstrcmp(argv[i],L"-l") == 0 )
		{
			if(  i + 1 < argc )
			{
				// Try to parse the number of characters
				int result = _wtoi(argv[i+1]);
				if( result >= 3 )
				{
					minCharacters = result;
				}else{
					fprintf(stderr,"Failed to parse -l argument. The string size must be 3 or larger:\n\teg. 'strings2 *.exe -l 6'\n");
					exit(0);
				}
				i++;
			}else{
				fprintf(stderr,"Failed to parse -l argument. It must be followed by a number:\n\teg. 'strings2 *.exe -l 6'\n");
				exit(0);
			}
		}else{
			// This is an unassigned argument
			if( filter == NULL )
			{
				filter = argv[i];
			}
			else
			{
				// This argument is an error, we already found our filter.
				fprintf(stderr,"Failed to parse argument number %i, '%S'. Try 'strings2 --help' for usage instructions.\n", i, argv[i]);
				exit(0);
			}
		}
	}

	// Fill out the options structure based on the flags
	STRING_OPTIONS options;

	options.min_chars = minCharacters;

	options.print_utf8 = flag_utf8_only || !flag_wide_string_only;
	options.print_wide_string = flag_wide_string_only || !flag_utf8_only;

	options.print_interesting = flag_strings_interesting;
	options.print_not_interesting = flag_strings_not_interesting;

	options.print_string_type = flag_print_string_type;
	options.print_filename = flag_filename;
	options.print_filepath = flag_filepath;

	options.json_file_output = wstring(L"");
	if (flag_json_file != NULL)
	{
		options.json_file_output = wstring(flag_json_file);
	}
	
	

	if( flag_help )
	{
		// Print help page
		printf("Strings2 is an improved version of the Sysinternals strings tool that extracts all unicode/ascii strings from binary data. On top of the classical strings approach, this version decodes multilingual strings (eg Chinese, Russian, etc) and uses a ML model to suppress noisy uninteresting strings.\n\n");
		printf("Example Usage:\n");
		printf("\tstrings2 malware.exe\n");
		printf("\tstrings2 *.exe > strings.txt\n");
		printf("\tstrings2 -pid 419 > process_strings.txt\n");
		printf("\tstrings2 -pid 0x1a3 > process_strings.txt\n");
		printf("\tstrings2 -system > all_process_strings.txt\n");
		printf("\tcat abcd.exe | strings2 > out.txt\n\n");
		printf("Flags:\n");
		printf(" -r\n\tRecursively process subdirectories.\n");
		printf(" -f\n\tPrints the filename/processname before each string.\n");
		printf(" -F\n\tPrints the full path and filename before each string.\n");
		printf(" -t\n\tPrints the string type before each string. UTF8,\n\tor WIDE_STRING.\n");
		printf(" -u\n\tPrints only WIDE_STRING strings that are encoded\n\tas two bytes per character.\n");
		printf(" -y\n\tPrints only UTF8 decoded strings.\n");
		printf(" -a\n\tPrints all strings including not interesting strings.\n");
		printf(" -ni\n\tPrints only not interesting strings.\n");
		printf(" -l [numchars]\n\tMinimum number of characters that is\n\ta valid string. Default is 4.\n");
		printf(" -pid\n\tThe strings from the process address space for the\n\tspecified PID will be dumped. Use a '0x' prefix to\n\tspecify a hex PID.\n");
		printf(" -system\n\tDumps strings from all accessible processes on the\n\tsystem. This takes awhile.\n");
		printf(" -json [file]\n\tWrites output to the specified json file.\n");
	}else{
		// Create the string parser object
		string_parser* parser = new string_parser(options);

		if (flag_dump_pid || flag_dump_system)
		{
			// Warn if running in 32 bit mode on a 64 bit OS
			if( is_win64() && sizeof(void*) == 4 )
			{
				fprintf(stderr, "WARNING: To properly dump address spaces of 64-bit processes the 64-bit version of strings2 should be used. Currently strings2 has been detected as running as a 32bit process under a 64bit operating system.\n\n");
			}

			// Elevate strings2 to the maximum privilges
			get_privileges( GetCurrentProcess() );

			// Create a process string dump class
			process_strings* process = new process_strings(parser);

			if( flag_dump_pid )
			{
				// Extract all strings from the specified process
				if( filter != NULL )
				{
					// Check the prefix
					bool isHex = false;
					wchar_t* prefix = new wchar_t[3];
					memcpy(prefix, filter, 4);
					prefix[2] = 0;

					if( wcscmp(prefix, L"0x") == 0 )
					{
						filter = &filter[2];
						isHex = true;
					}
					delete[] prefix;
					
					// Extract the pid from the string
					unsigned int PID;
					if( (isHex && swscanf(filter, L"%x", &PID) > 0) ||
						(!isHex && swscanf(filter, L"%i", &PID) > 0))
					{
						// Successfully parsed the PID
						
						// Parse the process
						process->dump_process(PID);

						
					}else{
						fwprintf(stderr, L"Failed to parse filter argument as a valid PID: %s.\n", filter);
					}
				}else{
					fwprintf(stderr, L"Error. No PID was specified. Example usage:\n\tstrings2 -pid 419 > process_strings.txt\n", filter);
				}
			}else if( flag_dump_system )
			{
				// Extract strings from the whole system
				process->dump_system();
			}

			delete process;
		}else if (piped_input)
		{
			// Set "stdin" to have binary mode:
			int result = _setmode( _fileno( stdin ), _O_BINARY );
			if( result == -1 )
				fprintf(stderr, "Failed to set piped data mode to binary but will continue with processing of piped data." );

			FILE* fh = fdopen(fileno(stdin), "rb");

			if( fh != NULL )
			{
				// Process the piped input
				parser->parse_stream(fh, "piped data");
				fclose(fh);
			}else{
				// Error
				fprintf(stderr, "Invalid stream: %s.\n", "Error opening the piped input: %s.\n", strerror(errno));
			}
		}else if(filter != NULL)
		{
			// Split the filter into the directory and filename filter halves
			char path[MAX_PATH+1] = {'.',0};
			wchar_t* last_slash = wcsrchr( filter, '\\' );
			if( last_slash == NULL || wcsrchr( filter, '/' ) > last_slash )
				last_slash = wcsrchr( filter, '/' );
			
			if( last_slash != NULL )
			{
				// Copy the path
				sprintf_s( path, MAX_PATH+1, "%S", filter );
				path[ last_slash - filter] = 0;

				// Move the filter
				memmove(filter, last_slash + 1, (wcslen(last_slash+1)+1)*2);
			}

			// Process the specified files
			process_folder( path, filter, flag_recursive, parser );
		}
		
		// Cleanup the string parser
		delete parser;
	}
	

	return 0;
}

