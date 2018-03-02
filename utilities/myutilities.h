/*******************************************************************************************************
*
*          Library created by Giovanni Garifo (giovanni.garifo@polito.it) that contains all the
*	       functions used to test, convert and do faboulous thing upon the input data.
*	       All the functions return a integer that
*          is 0 in case of FALSE, 1 in case of TRUE. -1 in case of failure.
*
*******************************************************************************************************/

/*
 * necessary libraries
 */
#include <arpa/inet.h> /*libreria richiest per check su correttezza IP, inet_aton ecc..*/
#include <stdlib.h> // getenv()
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h> // timeval
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <sys/un.h> // unix sockets
#include <netdb.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <netdb.h>

//aggiunti per INADRR_ANY
#include <netinet/in.h>
#include <netinet/ip.h>

//#include "../sockwrap.h"
//#include "../errlib.h"


/*
 * Costanti
 */
#define MAX_PORT 65535 //porta massima
#define MIN_PORT 1025 //porta minima, al di sotto sono utilizzabili solo da root

#define TCP 0 //usate in connectToHost per discriminare il tipo di connessione che vuole creare il client
#define UDP 1

#define CHUNK_SIZE 8000 //8KB

#define TRUE 1
#define FALSE 0

#define NOT_VALID 0
#define NOT_CLOSED -1
#define VALID 1

#define ERROR_ENCOUNTERED -1


/********************************************************************************************************************
 *                                           P R O T O T I P I                                                      *
 ********************************************************************************************************************/

//test dei dati in input dall'utente
int testIpAddress(char* ipAddress); //testa se l'IP ricevuto come stringa è compatibile con un indirizzo IPv4 (return 1 se valido)
int testIpAddress_v6(char* ipAddress); //testa se l'IP ricevuto è un IP v6 valido (return 1 se valido)
int testPort(int port); //verifica se la porta fa parte dell'intervallo ammissibile


//conversioni dati da Presentation Order (little endian) a NBO (big endian)
void convertIP(char* ipToConvert, struct in_addr* converted); //converte l'indirizzo IP da stringa a network byte order (unsigned long contenuto in struct in_addr)
void convertIP_v6(char* ipToConvert, struct in6_addr* converted);


//getters in formato stampabile
char* getPrintableIP(struct sockaddr_in from); //riceve la struct che identifica un host e ritorna l'IP dell'host remoto
int getPrintablePort(struct sockaddr_in from); //riceve la struct che identifica un host e ritorna la porta dell'host remoto


//utilità per lettura/scrittura su/da file
void writeChunk(FILE* fp, char* chunk, ssize_t nbytes); //scrive un blocco di byte su un file
size_t readChunk(FILE* fp, char* chunk, size_t nbytes); //legge da un blocco di byte da un file


/* Connessione mediante hostname/ip e porta: usata dal client per collegarsi al server */
int connectToHost (char* host, char* serv, int protocol_type);


//Handler per i segnali
void signal_sigpipe_handler(int signum);


//***TCP*** : Wrapper per delle funzioni di socket.h con gestione degli errori
int MyAccept (int listen_sockfd, struct sockaddr* cliaddr, socklen_t *addrlenp);
int MySelect (int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout);
ssize_t MyRecv (int fd, void *bufptr, size_t nbytes, int flags);
int MySend (int fd, void *bufptr, size_t nbytes, int flags); //nb: send originale ritorna void


//***UDP*** : Wrapper per le funzioni di ricezione/invio datagram, gestiscono gli errori
ssize_t MyRecvFrom (int fd, void *bufptr, size_t buf_len, int flags, struct sockaddr* remoteHostInfo, socklen_t* remoteHostInfo_len);
void MySendTo (int fd, void *bufptr, size_t buf_len, int flags, struct sockaddr* remoteHostInfo, socklen_t remoteHostInfo_len); 


ssize_t readNumBytes (int s, void* buf_ptr, size_t num_bytes); //Legge ""num_bytes"" dal buffer del socket connesso "s" in "buf_ptr"
int readChunkAndAppend( int s, FILE* fp, char* chunk, int filesize, ssize_t* totread ); //legge un blocco di byte dal socket connesso "s"" e li appende al file 




