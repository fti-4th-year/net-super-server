#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>

struct instance {
	int status;
	pid_t pid;
	int sockfd;
};

#define MAX_INST 0x100
#define ARGSLEN 0x10
#define STRLEN 0x40

struct port {
	int portno;
	int status;
	int sockfd;
	char *cfgstr;
	char *execstr;
	char *(args[ARGSLEN]);
	int ninst;
	int maxinst;
	struct instance inst[MAX_INST];
};

int done = 0;

void sighandler(int signo)
{
	done = 1;
}

#define MAX_PORTS 0x100

int main(int argc, char *argv[]) {
	int i, j;
	int nports;
	struct port port[MAX_PORTS];
	FILE *config;
	
	if(argc < 2) {
		printf("usage: %s <config-file>\n", argv[0]);
		return 0;
	}
	config = fopen(argv[1], "r");
	if(config == NULL) {
		fprintf(stderr, "cannot open '%s' : ", argv[1]);
		perror("");
		return -1;
	}
	
	for(i = 0; i < MAX_PORTS; ++i) {
		port[i].status = 1;
		port[i].cfgstr = NULL;
	}
	
	for(i = 0; i < MAX_PORTS; ++i) {
		int j;
		int portno;
		int ninst;
		char *execstr;
		char *line = NULL;
		size_t line_len = 0;
		
		if(getline(&line, &line_len, config) <= 0)
			break;
		port[i].cfgstr = line;
		
		int space = 0;
		char *sptr = line;
		int stage = 0, argp = 0;
		for(j = 0; j < line_len; ++j) {
			if(line[j] == '\0' || line[j] == '\n' || line[j] == ' ' || line[j] == '\t') {
				if(space) {
					// nothing
				} else {
					line[j] = '\0';
					if(stage == 0) {
						portno = atoi(sptr);
						++stage;
					} else if(stage == 1) {
						ninst = atoi(sptr);
						++stage;
					} else if(stage == 2) {
						execstr = sptr;
						++stage;
						port[i].args[argp] = sptr;
						++argp;
					} else if (stage == 3) {
						if(argp < ARGSLEN - 1) {
							port[i].args[argp] = sptr;
							++argp;
						}
					}
				}
				space = 1;
			} else {
				if(space) {
					sptr = line + j;
				} else {
					// nothing
				}
				space = 0;
			}
		}
		port[i].args[argp] = NULL;
		
		printf("%d %d %s ", portno, ninst, execstr);
		for(j = 0; port[i].args[j] != NULL; ++j) {
			printf("%s ", port[i].args[j]);
		}
		printf("\n");
		
		port[i].portno = portno;
		port[i].maxinst = ninst > 0 ? ninst : MAX_INST;
		port[i].execstr = execstr;
		nports = i + 1;
	}
	
	fclose(config);
	
	printf("server started\n");
	
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	
	for(int i = 0; i < nports; ++i) {
		int serv_sockfd;
		struct sockaddr_in serv_addr;
		int serv_portno = port[i].portno;
		
		port[i].status = 1;
		port[i].ninst = 0;
		
		for(int j = 0; j < MAX_INST; ++j) {
			port[i].inst[j].status = 1;
		}
		
		serv_sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if(serv_sockfd < 0) {
			perror("socket() error");
			continue;
		}
		
		port[i].sockfd = serv_sockfd;
		port[i].status = 0;
		
		bzero((char *) &serv_addr, sizeof(serv_addr));
		serv_addr.sin_family = AF_INET;
		serv_addr.sin_addr.s_addr = INADDR_ANY;
		serv_addr.sin_port = htons(serv_portno);
		
		if(bind(serv_sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
			fprintf(stderr, "bind(port=%d) error : ", serv_portno);
			perror("");
			close(serv_sockfd);
			port[i].status = 1;
			continue;
		}
		
		listen(serv_sockfd, 5);
	}
	
	while(!done) {
		int status;
		fd_set set;
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 10000;
		
		FD_ZERO(&set);
		int max_sockfd = 0;
		for(i = 0; i < nports; ++i) {
			int sockfd = port[i].sockfd;
			
			if(port[i].status)
				continue;
			
			FD_SET(sockfd, &set);
			if(sockfd > max_sockfd) {
				max_sockfd = sockfd;
			}
		}
		
		status = select(max_sockfd + 1, &set, NULL, NULL, &tv);
		if(status > 0) {
			for(i = 0; i < nports; ++i) {
				struct sockaddr_in cli_addr;
				socklen_t cli_len;
				int cli_sockfd = 0;
				int serv_sockfd = port[i].sockfd;
				int serv_portno = port[i].portno;
				pid_t pid;
				
				if(port[i].status || !FD_ISSET(serv_sockfd, &set))
					continue;
				
				cli_len = sizeof(cli_addr);
				cli_sockfd = accept(serv_sockfd, (struct sockaddr *) &cli_addr, &cli_len);
				if(cli_sockfd < 0) {
					fprintf(stderr, "accept(port=%d) error : ", serv_portno);
					perror("");
					continue;
				}
				
				if(port[i].ninst >= port[i].maxinst) {
					printf(
					  "%s:%d refused on port %d : limit %d is reached\n", 
					  inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port), 
					  serv_portno, port[i].maxinst
					  );
					close(cli_sockfd);
					continue;
				}
				
				for(j = 0; j < port[i].maxinst; ++j) {
					if(port[i].inst[j].status) {
						port[i].inst[j].sockfd = cli_sockfd;
						port[i].inst[j].status = 0;
						break;
					}
				}
				if(j >= port[i].maxinst) {
					fprintf(stderr, "no free instances. counter potentially broken");
					close(cli_sockfd);
					continue;
				}
				port[i].ninst++;
				
				printf(
				  "%s:%d accepted on port %d, total: %d\n", 
				  inet_ntoa(cli_addr.sin_addr), 
				  ntohs(cli_addr.sin_port), 
				  serv_portno, port[i].ninst
				  );
				
				pid = fork();
				if(pid < 0) {
					perror("fork() error");
					close(cli_sockfd);
					port[i].inst[j].status = 1;
					port[i].ninst--;
					continue;
				}
				if(pid != 0) {
					port[i].inst[j].pid = pid;
					printf("child %d started at port %d\n", pid, serv_portno);
				} else {
					char envaddr[STRLEN];
					char envport[STRLEN];
					snprintf(envaddr, STRLEN, "REMOTE_ADDR=%s", inet_ntoa(cli_addr.sin_addr));
					snprintf(envport, STRLEN, "REMOTE_PORT=%d", ntohs(cli_addr.sin_port));
					putenv(envaddr);
					putenv(envport);
					dup2(cli_sockfd, 0);
					dup2(cli_sockfd, 1);
					dup2(cli_sockfd, 2);
					status = execv(port[i].execstr, port[i].args);
					if(status < 0) {
						fprintf(stderr, "exec(\"%s\") error : ", port[i].execstr);
						perror("");
						return 0;
					}
				}
				
				
			}
		} else if(status < 0) {
			perror("select() error");
			continue;
		}
		
		for(i = 0; i < nports; ++i) {
			if(port[i].status)
				continue;
			for(j = 0; j < port[i].maxinst; ++j) {
				int wstat;
				if(port[i].inst[j].status)
					continue;
				status = waitpid(port[i].inst[j].pid, &wstat, WNOHANG);
				if(status < 0) {
					perror("wait() error");
					continue;
				} else if(status == 0)
					continue;
				
				close(port[i].inst[j].sockfd);
				port[i].inst[j].status = 1;
				port[i].ninst--;
				printf("child %d exited at port %d\n", port[i].inst[j].pid, port[i].portno);
			}
		}
	}
	
	for(i = 0; i < nports; ++i) {
		if(port[i].status)
			continue;
		for(j = 0; j < port[i].maxinst; ++j) {
			if(port[i].inst[j].status)
				continue;
			close(port[i].inst[j].sockfd);
			port[i].inst[j].status = 1;
		}
		close(port[i].sockfd);
		port[i].status = 1;
		if(port[i].cfgstr != NULL)
			free(port[i].cfgstr);
	}
	
	return 0;
}
