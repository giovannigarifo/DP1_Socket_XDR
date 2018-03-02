/*
 * Includes
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


//import dei prototipi necessari
#include "myutilities.h"
#include "sockwrap.h"
#include "errlib.h"



/********************************************************************************************************************
 *                                    Funzioni di utilità per conversioni e test                                    *
 ********************************************************************************************************************/

int testIpAddress(char* ipAddress) {
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ipAddress, &(sa.sin_addr)); //return 1 if correct IP
    return result;
}

int testIpAddress_v6(char* ipAddress) {
    struct sockaddr_in6 sa;
    int result = inet_pton(AF_INET6, ipAddress, &(sa.sin6_addr)); //return 1 if correct IP
    return result;
}

int testPort(int port) {
    return port <= MAX_PORT && port >= MIN_PORT; //return 1 if relation is valid, so port is valid
}

void convertIP(char* ipToConvert, struct in_addr* converted) {

    if ( inet_pton(AF_INET, ipToConvert, converted) != 1 ){

        printf("[error in function convertIP] unable to convert the ipaddress from presentation (text) to binary.");
        exit(EXIT_FAILURE);
    }
};

void convertIP_v6(char* ipToConvert, struct in6_addr* converted) {

    if ( inet_pton(AF_INET6, ipToConvert, converted) != 1 ){

        printf("[error in function convertIP] unable to convert the ipaddress from presentation (text) to binary.");
        exit(EXIT_FAILURE);
    }
};

char* getPrintableIP(struct sockaddr_in from){
    return inet_ntoa(from.sin_addr);
}

int getPrintablePort(struct sockaddr_in from){
    return ntohs(from.sin_port);
}



/***************************************************************************************
*                  Wrapper delle funzioni per l'I/O da FILE                            *
****************************************************************************************/

/***
 * LEGGE un blocco di dati da un FILE
 * @param fp : puntatore al file
 * @param chunk : array inizializzato a lunghezza fissa su cui vengono salvati i dati
 * @param nbytes : lunghezza dell'array chunk
 * @return numero di byte effettivamente letti
 */
size_t readChunk(FILE* fp, char* chunk, size_t nbytes){

    size_t read_size = 0;
    
   /* 
    printf("\nreadChunk is ready\n");
    fflush(stdout);
	*/

    if(fp){
        read_size = fread(chunk, sizeof(char), nbytes, fp);
    }
	
	if(ferror(fp)){
		printf("[error] unable to read chunk from file. exiting with failure state.\n");
		exit(EXIT_FAILURE);
	}
	
	/*
	printf("[debug] readChunk read %lu bytes of %lu.\n", read_size, nbytes);
	fflush(stdout);
	*/
	
    return read_size; //numero di byte effettivammente letti.
}

/***
 * SCRIVE un blocco di dati sul FILE
 * @param fp : puntatore al file
 * @param chunk : array inizializzato a lunghezza fissa su cui vengono salvati i dati
 * @param nbytes : lunghezza dell'array chunk
 * @return numero di byte effettivamente letti
 */
void writeChunk(FILE* fp, char* chunk, ssize_t nbytes){

    size_t write_size;

    if(fp){

        write_size = fwrite(chunk, sizeof(char), nbytes, fp);

        if(write_size != nbytes){

            //qui si dovrebbe riprovare a scrivere!

            printf("[error] unable to append to file. exiting with failure state.\n");
            exit(EXIT_FAILURE);
        }
    }

    return;
}



/***************************************************************************************
*                  Wrapper delle funzioni di gestione dei SOCKET con error handling
****************************************************************************************/


/*********************************************************
 * Connessione al server TCP/UDP mediante hostname o indirizzo IP
 *
 * host : hostname o ipv4 addres
 * serv : nome standard protocollo o numero di porta
 * protocol_type : 0 per TCP ed 1 per UDP
 * 
 * Ritorna: socket creato
 */
int connectToHost (char* host, char* serv, int protocol_type) {
	
	struct addrinfo hints, *res, *res0;
	int errorCode, s; 
	
	//inizializza struttura hints: contiene info sul tipo di connessione desiderato
	memset(&hints, 0, sizeof(hints));
	
	if(protocol_type == TCP) {
		
		hints.ai_family = PF_INET; //mi va bene solo IPv4
		hints.ai_socktype = SOCK_STREAM; //socket tcp
		
	} else {
		
		hints.ai_family = PF_INET; //mi va bene solo IPv4
		hints.ai_socktype = SOCK_DGRAM; //socket UDP
	}
	
	
	//chiamata getaddrinfo per ottenere lista di possibili indirizzi/porte
	if ( (errorCode = getaddrinfo(host, serv, &hints, &res0)) ){ 
		
		printf("[error] getaddrinfo failed with error code: %d.\nExiting...\n", errorCode);
		exit(EXIT_FAILURE);	
	}
	
	/*
	 * Effettua ricerca all'interno della lista ritornata da getaddrinfo 
	 */
	s = -1;
		
	for (res = res0; res!=NULL; res = res->ai_next) {
		
		//prova a creare socket
		s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (s < 0) {
			continue;
		}
		
		//prova a connetterti
		if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
			close(s);
			s = -1;
			continue;
		}	
		
		break; /* okay we got one */
	}
	
	freeaddrinfo(res0); /* free list of structures */
	
	if (s < 0) {
		printf("[error] getaddrinfo didn t found any possible IP address for the hostname.\nExiting...\n");
		exit(EXIT_FAILURE);	
	}
	
	return s;
}



/**
 * 
 * 		T C P
 * 
 **/

//accept
int MyAccept (int listen_sockfd, struct sockaddr* cliaddr, socklen_t* addrlenp)
{
    int n;

    again:
    if ( (n = accept(listen_sockfd, cliaddr, addrlenp)) < 0)
    {
        if (errno == EPROTO || errno == ECONNABORTED ||
            errno == EMFILE || errno == ENFILE ||
            errno == ENOBUFS || errno == ENOMEM )
            goto again;

        else{
            return -1; //ritorna -1 invece di fare exit, server si pone nuovamente in attesa
        }

    }

    return n;
}

//select
int MySelect (int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout)
{
    int n;

    if ( (n = select (maxfdp1, readset, writeset, exceptset, timeout)) < 0)
       return -1; //ritorna -1 invece di fare exit, server si pone nuovamente in attesa

    return n;
}

//recv
ssize_t MyRecv (int fd, void *bufptr, size_t nbytes, int flags){

    ssize_t n;

    if ( (n = recv(fd,bufptr,nbytes,flags)) < 0)
        return -1;
	
    return n;
}

//send
int MySend (int fd, void *bufptr, size_t nbytes, int flags)
{
    ssize_t n;

    if ( (n = send(fd,bufptr,nbytes,flags)) != (ssize_t)nbytes)
        return -1; //se non riesco ad inviare tutti i byte ritorno errore

    return n;
}



/**
 * 
 * 		U D P
 * 
 **/

//sendto
void MySendTo (int fd, void *bufptr, size_t buf_len, int flags, struct sockaddr* remoteHostInfo, socklen_t remoteHostInfo_len){
	
	int nsent = 0;
	
	nsent = sendto(fd, bufptr, buf_len, 0, remoteHostInfo, remoteHostInfo_len);
	
	if( nsent != buf_len ){
        printf("[warning] unable to send the entire datagram of %d bytes, just %d byte sended (-1 means error). errno=%d\n", (int)buf_len, (int)nsent, errno);
        //no exit perchè UDP non critico  
    }
}


//recvfrom
ssize_t MyRecvFrom(int fd, void *bufptr, size_t buf_len, int flags, struct sockaddr* remoteHostInfo, socklen_t* remoteHostInfo_len){
	
	int nreceived = 0; 
	
    nreceived = recvfrom(fd, bufptr, buf_len, flags, remoteHostInfo, remoteHostInfo_len); //si blocca qui in attesa di un datagram
        
    if( nreceived == -1 ) {
        
        printf("[warning] unable to receive data. recvfrom returned: %d.\n", nreceived);
        exit(EXIT_FAILURE);
    
    } else return nreceived; //numero di byte effettivamente letti dal payload del datagram ricevuto (lunghezza contenuto in buffer)
}




/***
 * Legge num_bytes dal socket s in buf_ptr
 * 
 * @param s : filedescriptor del Socket Conesso
 * @param buf_ptr : array di byte su cui scrivere i dati letti
 * @param num_bytes : numero di byte da leggere in buf_ptr
 * @return numero di byte effettivamente letti
 */
ssize_t readNumBytes (int s, void* buf_ptr, size_t num_bytes){

    ssize_t nread = 0;
    ssize_t tot_read = 0;
    size_t remaning_bytes = num_bytes;

    while (remaning_bytes > 0){

        nread = MyRecv(s, buf_ptr, remaning_bytes, 0); //provo a leggere dal socket nbytes

        if( nread == ERROR_ENCOUNTERED){
			
            printf("[debug] recv inside readNumBytes() failed (return value = -1).\n");
            exit(EXIT_FAILURE);

        } else if (nread > 0) {
            remaning_bytes -= nread; //decremento del numero di byte effettivamente letti a questo giro
            tot_read += nread; //aggiorno totale dei byte letti fin'ora
            buf_ptr += nread; //sposto puntatore in avanti

        } else if (nread == 0) {
            //connection closed by remote host
            break;
        }
    }

    return tot_read;//ritorno il totale dei byte letti
}


/*
 * Legge un blocco di byte dal socket e li appende al file 
 */
int readChunkAndAppend(int s, FILE* fp, char* chunk, int filesize, ssize_t* totread){

    ssize_t nread;

    memset(chunk, 0, CHUNK_SIZE); //pulisco zona memoria dove salvare i dati

    if( filesize>=CHUNK_SIZE && *totread+CHUNK_SIZE>filesize ){

            //sto leggendo l'ultimo pezzo di un file più grande di un chunk
            nread = readNumBytes(s, chunk, (size_t)(filesize - *totread));

    }else if( filesize >= CHUNK_SIZE ) {

            //devo leggere un chunk di mezzo di un file grande
            nread = readNumBytes(s, chunk, CHUNK_SIZE);

    } else {
            //leggo tutto la prima volta, il file è più piccolo di un chunk
            nread = readNumBytes(s, chunk, filesize);
    }


    if( nread>0 ){
            //letto un pezzo di file, appendo
            writeChunk(fp, chunk, nread);
            *totread += nread; //numero totale di byte letti dal socket

    } else return ERROR_ENCOUNTERED;

    return nread; //se ho letto correttamente il chunk e scritto correttamente il file
}



/***************************************************************************************
*                           HANDLER PER I SEGNALI
****************************************************************************************/

//handler per SIGPIPE
void signal_sigpipe_handler(int signum){
	
    printf("Caught signal SIGPIPE (signum=%d). The host closed the connection unexpectedly, maybe it failed.\n",signum);
    fflush(stdout);
}


