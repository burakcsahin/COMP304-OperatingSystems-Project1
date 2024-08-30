#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h> // termios, TCSANOW, ECHO, ICANON
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

const char *sysname = "mishell";

enum return_codes {
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};

struct command_t {
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3]; // in/out redirection
	struct command_t *next; // for piping
};

bool kernelLoaded = false;

/**
 * Prints a command struct
 * @param struct command_t *
 */

// Function declarations
int execute_hdiff(struct command_t *command);
void compareTextFiles(const char *file1, const char *file2);
void compareBinaryFiles(const char *file1, const char *file2);
int mkdir_command(struct command_t *command);
int rmdir_command(struct command_t *command);
int execute_countlines(struct command_t *command);
int execute_scoutword(struct command_t *command);
int execute_psvis(struct command_t *command);
int clear_kernel_log();
int print_kernel_log();

void print_command(struct command_t *command) {
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n",
		   command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");

	for (i = 0; i < 3; i++) {
		printf("\t\t%d: %s\n", i,
			   command->redirects[i] ? command->redirects[i] : "N/A");
	}

	printf("\tArguments (%d):\n", command->arg_count);

	for (i = 0; i < command->arg_count; ++i) {
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	}

	if (command->next) {
		printf("\tPiped to:\n");
		print_command(command->next);
	}
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command) {
	if (command->arg_count) {
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}

	for (int i = 0; i < 3; ++i) {
		if (command->redirects[i])
			free(command->redirects[i]);
	}

	if (command->next) {
		free_command(command->next);
		command->next = NULL;
	}

	free(command->name);
	free(command);
	return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt() {
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command) {
	const char *splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);

	// trim left whitespace
	while (len > 0 && strchr(splitters, buf[0]) != NULL) {
		buf++;
		len--;
	}

	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL) {
		// trim right whitespace
		buf[--len] = 0;
	}

	// auto-complete
	if (len > 0 && buf[len - 1] == '?') {
		command->auto_complete = true;
	}

	// background
	if (len > 0 && buf[len - 1] == '&') {
		command->background = true;
	}

	char *pch = strtok(buf, splitters);
	if (pch == NULL) {
		command->name = (char *)malloc(1);
		command->name[0] = 0;
	} else {
		command->name = (char *)malloc(strlen(pch) + 1);
		strcpy(command->name, pch);
	}

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;

	while (1) {
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch)
			break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		// empty arg, go for next
		if (len == 0) {
			continue;
		}

		// trim left whitespace
		while (len > 0 && strchr(splitters, arg[0]) != NULL) {
			arg++;
			len--;
		}

		// trim right whitespace
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL) {
			arg[--len] = 0;
		}

		// empty arg, go for next
		if (len == 0) {
			continue;
		}

		// piping to another command
		if (strcmp(arg, "|") == 0) {
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t')
				index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0) {
			// handled before
			continue;
		}

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<') {
			redirect_index = 0;
		}

		if (arg[0] == '>') {
			if (len > 1 && arg[1] == '>') {
				redirect_index = 2;
				arg++;
				len--;
			} else {
				redirect_index = 1;
			}
		}

		if (redirect_index != -1) {
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 &&
			((arg[0] == '"' && arg[len - 1] == '"') ||
			 (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}

		command->args =
			(char **)realloc(command->args, sizeof(char *) * (arg_index + 1));

		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;

	// increase args size by 2
	command->args = (char **)realloc(
		command->args, sizeof(char *) * (command->arg_count += 2));

	// shift everything forward by 1
	for (int i = command->arg_count - 2; i > 0; --i) {
		command->args[i] = command->args[i - 1];
	}

	// set args[0] as a copy of name
	command->args[0] = strdup(command->name);

	// set args[arg_count-1] (last) to NULL
	command->args[command->arg_count - 1] = NULL;

	return 0;
}

void prompt_backspace() {
	putchar(8); // go back 1
	putchar(' '); // write empty over
	putchar(8); // go back 1 again
}

int process_command(struct command_t *command);

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command) {
	size_t index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &=
		~(ICANON |
		  ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	show_prompt();
	buf[0] = 0;

	while (1) {
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		// handle tab
		if (c == 9) {
                        
                        char* all_files[10000];
                        DIR *dir;
                        struct dirent *file;
                        int num = 0;
                        dir = opendir("/bin");
                        char copy[5000];
                        strcpy(copy,buf);
                        int next_idx = index;
                        copy[next_idx] = '\0';
                        const char* built_ins[] = {"cd", "exit", "hdiff", "countlines", "scoutword","psvis"};
                        bool perfect_match = false;

                        while((file = readdir(dir)) != NULL){
                                if(file->d_type == DT_REG){
                                        if(strcmp(copy, file->d_name) == 0){
                                                perfect_match = true;
                                                break;
                                        }
                                        if(strncmp(copy, file->d_name, strlen(copy))== 0){
                                                all_files[num] = strdup(file->d_name);
                                                num++;
                                        }
                                }
                        }

                        for(size_t i = 0; i < sizeof(built_ins) / sizeof(built_ins[0]) ; i++){
                                if(strcmp(copy, built_ins[i]) == 0){
                                        perfect_match = true;
                                        break;
                                }
                                if(strncmp(copy, built_ins[i], strlen(copy)) == 0){
                                        all_files[num] = strdup(built_ins[i]);
                                        num++;
                                }
                        }

                        closedir(dir);
                        
                        if(perfect_match){
				printf("\n");
				char newbuf[3] = {'l','s','\0'};
				parse_command(newbuf,command);
				process_command(command);				
                        }
                        else{
                                if(num > 1){
                                        for(int i = 0; i < num; i++){
                                                printf("\n%s\n",all_files[i]);
                                        }
                                }
                                else{
                                        while (index > 0){
                                                prompt_backspace();
                                                index--;
                                        }
                                        printf("%s", all_files[0]);
                                        index += strlen(all_files[0]);
                                        strcpy(buf,all_files[0]);
                                        continue;
                                }
                        }

                        buf[index++] = '?'; // autocomplete
                        break;
                }

		// handle backspace
		if (c == 127) {
			if (index > 0) {
				prompt_backspace();
				index--;
			}
			continue;
		}

		if (c == 27 || c == 91 || c == 66 || c == 67 || c == 68) {
			continue;
		}

		// up arrow
		if (c == 65) {
			while (index > 0) {
				prompt_backspace();
				index--;
			}

			char tmpbuf[4096];
			printf("%s", oldbuf);
			strcpy(tmpbuf, buf);
			strcpy(buf, oldbuf);
			strcpy(oldbuf, tmpbuf);
			index += strlen(buf);
			continue;
		}

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1)
			break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}

	// trim newline from the end
	if (index > 0 && buf[index - 1] == '\n') {
		index--;
	}

	// null terminate string
	buf[index++] = '\0';

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	//print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}



int main() {
	while (1) {
		struct command_t *command = malloc(sizeof(struct command_t));

		// set all bytes to 0
		memset(command, 0, sizeof(struct command_t));

		int code;
		code = prompt(command);
		if (code == EXIT) {
			break;
		}

		code = process_command(command);
		if (code == EXIT) {
			break;
		}

		free_command(command);
	}

	printf("\n");
	return 0;
}

int process_command(struct command_t *command) {
	int r;

	if (strcmp(command->name, "") == 0) {
		return SUCCESS;
	}

	if (strcmp(command->name, "exit") == 0) {

		if(kernelLoaded){
        		char* args[] = {"/bin/sudo", "rmmod", "module/mymodule.ko", NULL};
        		pid_t pid = fork();

        		if(pid==0){
                		execv(args[0],args);
                		exit(0);
        		}
        		else{
                		wait(0);
        		}

			kernelLoaded = false;
		}
		return EXIT;
	}

	if (strcmp(command->name, "cd") == 0) {
		if (command->arg_count > 0) {
			r = chdir(command->args[1]);
			if (r == -1) {
				printf("-%s: %s: %s\n", sysname, command->name,
					   strerror(errno));
			}

			return SUCCESS;
		}
	}
	if (strcmp(command->name, "hdiff") == 0) {
        	return execute_hdiff(command);
    	}
	 if (strcmp(command->name, "mkdir") == 0) {
        	return mkdir_command(command);
    	}

   	 if (strcmp(command->name, "rmdir") == 0) {
        	return rmdir_command(command);
    	}
	 if (strcmp(command->name, "countlines") == 0) {
    		return execute_countlines(command);
	}
	 if (strcmp(command->name, "scoutword") == 0) {
    		return execute_scoutword(command);
	}
	 if(strcmp(command->name, "psvis") == 0){
		 return execute_psvis(command);
	 }

	pid_t pid = fork();
	// child
	if (pid == 0) {
		/// This shows how to do exec with environ (but is not available on MacOs)
		// extern char** environ; // environment variables
		// execvpe(command->name, command->args, environ); // exec+args+path+environ

		/// This shows how to do exec with auto-path resolve
		// add a NULL argument to the end of args, and the name to the beginning
		// as required by exec

		// TODO: do your own exec with path resolving using execv()
		// do so by replacing the execvp call below

		//command->args = (char**) realloc(command->args, sizeof(char*) * (command->arg_count+=1));
		//command->args[command->arg_count-1] = NULL;

		while(command->next != NULL){
			int pipes[2];
			if(pipe(pipes) <0){
				perror("Pipe error");
			}

			pid_t pid = fork();
			if(pid == 0){
				close(pipes[0]);
				dup2(pipes[1],1);
				char path[99] = "/bin/";
				strcat(path,command->name);
				execv(path, command->args);
				exit(0);
			}
			else{
				close(pipes[1]);
				dup2(pipes[0],0);
				command = command->next;
			}
		}

		char path[99] = "/bin/";
		strcat(path,command->name);
		execv(path, command->args); // exec+args+path
		exit(0);
	} else {
		// TODO: implement background processes here
		if(!command->background){
			wait(0);
		}// wait for child process to finish
		return SUCCESS;
	}

	// TODO: your implementation here

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}

int execute_psvis(struct command_t *command){
	//executing psvis by loading the module to the kernel
	if(command->arg_count != 3){
		printf("Invalid argument number!\n");
		return UNKNOWN;
	}
	int PID = atoi(command->args[1]);

	if(PID <= 0){
		printf("Invalid parameter!\n");
		return UNKNOWN;
	}

	clear_kernel_log();

	char inputPID[30];
	sprintf(inputPID,"PID=%d",PID);

	char* args[] = {"/bin/sudo", "insmod", "module/mymodule.ko", inputPID, NULL};
	pid_t pid = fork();

	if(pid==0){
		execv(args[0],args);
		exit(0);
	}
	else{
		wait(0);
	}
	
	kernelLoaded = true;

	print_kernel_log();

	return SUCCESS;
}

int clear_kernel_log(){
	//clear kernel log
	char* args[] = {"/bin/sudo", "dmesg", "-C", NULL};
	pid_t pid = fork();
	if(pid==0){
		execv(args[0],args);
		exit(0);
	}
	else{
		wait(0);
	}

	return SUCCESS;
}

int print_kernel_log(){
	//print kernel log after psvis call
        char* args[] = {"/bin/sudo", "dmesg", NULL};
        pid_t pid = fork();
        if(pid==0){
                execv(args[0],args);
                exit(0);
        }       
        else{
                wait(0);
        }       
        
        return SUCCESS;
} 

int execute_hdiff(struct command_t *command) {
    // Check if correct number of arguments provided
    if (command->arg_count != 5) {
        printf("Usage: hdiff [-a | -b] file1 file2\n");
        return UNKNOWN;
    }

    // Determine the mode
    int mode = 0; // 0: text mode, 1: binary mode
    if (strcmp(command->args[1], "-b") == 0) {
        mode = 1;
    } else if (strcmp(command->args[1], "-a") != 0) {
        printf("Error: Invalid mode\n");
        return UNKNOWN;
    }

    // Compare the files based on the mode
    if (mode == 0) {
        compareTextFiles(command->args[2], command->args[3]);
    } else {
        compareBinaryFiles(command->args[2], command->args[3]);
    }

    return SUCCESS;
}

void compareTextFiles(const char *file1, const char *file2) {
    FILE *file1_ptr = fopen(file1, "r");
    FILE *file2_ptr = fopen(file2, "r");

    if (file1_ptr == NULL || file2_ptr == NULL) {
        perror("Error opening files");
        return;
    }

    char *line1 = NULL, *line2 = NULL;
    size_t len1 = 0, len2 = 0;
    ssize_t read1, read2;
    int lineNum = 0;
    int diffLineCount = 0;

    while ((read1 = getline(&line1, &len1, file1_ptr)) != -1 && (read2 = getline(&line2, &len2, file2_ptr)) != -1) {
        if (strcmp(line1, line2) != 0) {
            printf("%s:Line %d: %s", file1, lineNum, line1);
            printf("%s:Line %d: %s", file2, lineNum, line2);
            diffLineCount++;
        }
        lineNum++;
    }

    if (diffLineCount == 0)
        printf("The two files are identical.\n");
    else
        printf("%d different lines found.\n", diffLineCount);

    free(line1);
    free(line2);
    fclose(file1_ptr);
    fclose(file2_ptr);
}

void compareBinaryFiles(const char *file1, const char *file2) {
    FILE *file1_ptr = fopen(file1, "rb");
    FILE *file2_ptr = fopen(file2, "rb");

    if (file1_ptr == NULL || file2_ptr == NULL) {
        perror("Error opening files");
        return;
    }

    int totalByteDiff = 0;
    int byte1, byte2;

    while ((byte1 = fgetc(file1_ptr)) != EOF && (byte2 = fgetc(file2_ptr)) != EOF) {
        if (byte1 != byte2) {
            totalByteDiff++;
        }
    }

    if (totalByteDiff > 0)
        printf("%d bytes are different.\n", totalByteDiff);
    else
        printf("The two files are identical.\n");

    fclose(file1_ptr);
    fclose(file2_ptr);
}

// Function to execute the mkdir command
int mkdir_command(struct command_t *command) {
    if (command->arg_count != 3) {
        printf("Usage: mkdir <directory_name>\n");
        return UNKNOWN;
    }

    int status = mkdir(command->args[1], 0777);
    if (status == -1) {
        perror("mkdir");
        return UNKNOWN;
    }

    printf("Directory '%s' created successfully.\n", command->args[1]);
    return SUCCESS;
}

// Function to execute the rmdir command
int rmdir_command(struct command_t *command) {
    if (command->arg_count != 3) {
        printf("Usage: rmdir <directory_name>\n");
        return UNKNOWN;
    }

    int status = rmdir(command->args[1]);
    if (status == -1) {
        perror("rmdir");
        return UNKNOWN;
    }

    printf("Directory '%s' removed successfully.\n", command->args[1]);
    return SUCCESS;
}

int execute_countlines(struct command_t *command) {
    // Check if correct number of arguments provided
    if (command->arg_count != 3) {
        printf("Usage: countlines <file>\n");
        return UNKNOWN;
    }

    // Open the file
    FILE *file_ptr = fopen(command->args[1], "r");
    if (file_ptr == NULL) {
        perror("Error opening file");
        return UNKNOWN;
    }

    // Count the lines
    int line_count = 0;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), file_ptr) != NULL) {
        line_count++;
    }

    // Close the file
    fclose(file_ptr);

    // Print the result
    printf("Number of lines in %s: %d\n", command->args[1], line_count);

    return SUCCESS;
}
int execute_scoutword(struct command_t *command) {
    // Check if correct number of arguments provided
    if (command->arg_count != 4) {
        printf("Usage: scoutword <word> <file>\n");
        return UNKNOWN;
    }

    // Open the file
    FILE *file_ptr = fopen(command->args[2], "r");
    if (file_ptr == NULL) {
        perror("Error opening file");
        return UNKNOWN;
    }

    // Read the word to search for
    char *search_word = command->args[1];

    // Count the occurrences
    int occurrence_count = 0;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), file_ptr) != NULL) {
        char *pos = buffer;
        while ((pos = strstr(pos, search_word)) != NULL) {
            occurrence_count++;
            pos += strlen(search_word);
        }
    }

    // Close the file
    fclose(file_ptr);

    // Print the result
    if (occurrence_count > 0) {
        printf("Occurrences of '%s' in %s: %d\n", search_word, command->args[2], occurrence_count);
    } else {
        printf("The file '%s' does not contain the word '%s'\n", command->args[2], search_word);
    }

    return SUCCESS;
}
