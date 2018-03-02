/************************************************************************************
*
*   SERVER TCP CONCORRENTE CON PREALLOCAZIONE e DUAL PROTOCOL (ASCII/XDR)
*
*   Server TCP concorrente per SCAMBIO FILE
*   - (1) si pone in ascolto su porta ricevuta da command line
*   - (2) riceve richiesta di trasferimento file
*   - (3) trasmette il file all'host
*
*   Protocollo (ASCII):
 *
 *  Messaggi che si possono ricevere:
 *   richiesta file: "GET filename\r\n"
 *   chiusura comunicazione da parte del client: "QUIT\r\n"
 *
 *  Risposte:
 *   invio file: "+OK\r\nB1B2B3B4T1T2T3T4filecontent", dove B1B2B3B4 e T1T2T3T4 sono quadruple di byte
 *   che contengono il numero di byte del file da trasferire ed il timestamp dell'ultima modifica del file.
 *
 *   errore: "-ERR\r\n"
*
*************************************************************************************/

/*
*	Includes 
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h> //per poter settare i signal handler definiti in myutilities.h
/*librerie richieste per l'utilizzo dei socket in c*/
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h> //per timeouts
#include <netinet/in.h> //per Internet
#include <unistd.h> //per close
/*librerie richieste per stat e fstats*/
#include <sys/stat.h>
/*libreria richista da mmap*/
#include <sys/mman.h>
#include <fcntl.h>

/*utilities*/
#include "../utilities/myutilities.h" //mia libreria che contiene funzioni di utilità
#include "../utilities/sockwrap.h" //wrappers with error handling for socket functions

/*xdr*/
#include "../xdr_types/types.h" //includo tipi XDR

/*
*	Constants
*/
#define NOT_VALID 0
#define LISTENQ 15 //numero massimo di host che si possono porre in attesa, gli altri ricevono connection refused
#define QUIT_REQUESTED 0
#define WRONG_COMMAND -1
#define ERROR_ENCOUNTERED -1
#define CLIENT_SERVED 1
#define MAX_FILENAME_LENGTH 50
#define WAIT_TIME 20 //tempo per cui si attende comando dal client prima di chiudere la connessione
#define NUM_WORKERS 3 //numero di processi che vengono forkati 
#define ENABLED 1
#define DISABLED 0


/************************
* 		Prototypes
*************************/
void workerJob(int s); //funzione eseguita da ogni processo forkato

int serveRemoteHost(int conn_s, struct sockaddr_in remoteHostInfo, int remoteHostInfoLen); //legge comando ricevuto sul socket (codifica binaria) ed intraprende azione relativa
int serveRemoteHostXDR(int conn_s, FILE* stream_socket_r, FILE* stream_socket_w, struct sockaddr_in remoteHostInfo, int remoteHostInfoLen); //legge comando ricevuto sul socket (codifica xdr) ed intraprende azione relativa

//funzioni che implementano l'azione richiesta corrispondente ad un comando
int sendFile(int conn_s, struct sockaddr_in remoteHostInfo, int remoteHostInfoLen, char* filename);
int sendFileXDR_mmap(int conn_s, FILE* stream_socket_r, FILE* stream_socket_w, struct sockaddr_in remoteHostInfo, int remoteHostInfoLen, char* filename);

//invio messaggio di errore al client per svariate cause
void sendErrMsg(int conn_s);
void sendErrMsgXDR(int conn_s, FILE* stream_socket_w);

/************************
*	Global Variables
*************************/
char* prog_name; //name del programma letto da argv
int XDR_MODE = DISABLED; //modalità di codifica/decodifica, letta da argv


/**********************************
 *  			Main
 **********************************/
int main(int argc, char** argv)
{

    int s; //file descriptor del socket
    
    //socket
    struct sockaddr_in6 sockInfo_v6; //struttura dati che contiene le informazioni sul socket creato dal server
    int sockInfoLen_v6; //lunghezza della struct di sopra
    struct in6_addr ipAddress_v6; //ip address in network byte order
    short port; //porta su cui porsi in ascolto
    
    //processes
    int workers_id[NUM_WORKERS]; //array che contiene i PID dei workers
    int worker_exit_status;
    
    int argc_offset = 0;

   /*********************************
    *   (1) Convalida Input
    **********************************
    *   - argv[0] nome del programma
    *   - argv[1] -> OPZIONALE: -x se abilitato XDR
    *   - argv[2] -> porta su cui il server è in ascolto
    */
    if( argc <2 || argc>3 ){

        printf("[error] numero dei parametri errato, sintassi corretta: $ %s [-x] binding_port\n\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    //calcolo offset degli argomenti se presente il parametro -x
    if(argc == 2){
		
        argc_offset = 0;
		XDR_MODE = DISABLED;
		
    } else if(argc == 3){
		
        argc_offset = 1;

        //test su parametro opzionale
        if( strlen(argv[1]) == 2 && argv[1][0] == '-' && argv[1][1] == 'x' )
            printf("\nAbilitata funzionalità XDR. i dati verranno scambiati secondo la codifica XDR.\n\n");
        else {
            printf("Errore nel comando opzionale -x\n");
            exit(EXIT_FAILURE);
        }

        XDR_MODE = ENABLED;
    }

    if ( testPort( atoi(argv[1 + argc_offset])) == NOT_VALID ) {

        printf("[error] porta inserita non valida: %s, intervallo utilizzabile: 1025-65535\n", argv[2]);
        exit(EXIT_FAILURE);
    }

    port = atoi(argv[1 + argc_offset]); //would always fit thanks to previous test
    prog_name = argv[0];

    /************************************************************
    * Creazione del Socket Dual Stack w/ IPv4-mapped compability
    */

    //inizializza a 0 la porzione di memoria occupata dalla struct sockaddr_in6
    memset(&sockInfo_v6, 0, sizeof(sockInfo_v6));

    //convert IP from presentation to Network Byte Order (unsigned long)
	convertIP_v6("::", &ipAddress_v6);
    
    //popolamento struttura sockInfo_v6
    sockInfo_v6.sin6_family = AF_INET6;
    sockInfo_v6.sin6_addr = ipAddress_v6;
    sockInfo_v6.sin6_port = htons(port);
    sockInfoLen_v6 = sizeof(sockInfo_v6);

    //Creazione socket Dual Stack w/ ipv4-mapped 
    s = Socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);

    //binding alla porta TCP
    Bind(s, (struct sockaddr*) &sockInfo_v6, sockInfoLen_v6);

    //server si pone in ascolto:
    Listen(s, LISTENQ);

    /***
     * registring signal handlers
     */
    signal(SIGPIPE, signal_sigpipe_handler); //registro handler


    /***
     * Main process forks NUM_WORKERS childrens that will handle the connection, and then keep waiting for them to terminate.
     */
    for(int i = 0; i < NUM_WORKERS; i++){

        workers_id[i] = fork(); //FORGE WORKERS

        if(workers_id[i] == 0){
            workerJob(s); //slave process go here, start serving the clients
        }
    }

    /***
     * Main now wait for workers to exit (it only happens in case of fatal failure)
     */
    while(1) {

        int pid = wait(&worker_exit_status); //main process blocks here
        printf("[debug] one of the workers unexpectedly exited with status %d...forking a new worker.\n", worker_exit_status);

        int worker_id = fork();

        if(!worker_id){
            workerJob(s); //child, go listen for connections
        }

        //sostituisco pid del worker deceduto con pid del nuovo worker
        for(int i=0; i< NUM_WORKERS; i++){
            if(workers_id[i] == pid) workers_id[i] = worker_id;
        }
    };

//end of main
}


/*************************************************
*                F U N C T I O N S 
**************************************************/


/*******************
 * WORKER FUNCTION
 * 
 * Ogni worker esegue questa funzione indefinitivamente: loop in attesa di accettare connessioni
 */
void workerJob(int s){

    //strutture dati
    int conn_s; //file descriptor del socket connesso
    struct sockaddr_in remoteHostInfo; //info su host remoto
    int remoteHostInfoLen = sizeof(remoteHostInfo);
    
    FILE* stream_socket_r; //fp collegato al socket connesso in lettura
    FILE* stream_socket_w; //fp collegato al socket connesso in scrittura
    

    /**********************************************
    *   Start waiting for incoming connections
    */
    while(1) {
		
        printf("Worker %d - waiting for a connection...\n", getpid());

        //wait for a connection
        conn_s = MyAccept(s, (struct sockaddr*) &remoteHostInfo, (socklen_t*)&remoteHostInfoLen);
        if(conn_s == ERROR_ENCOUNTERED){ 
			continue; //poniti nuovamente in ascolto
		} 
        
        //se modalità XDR, apro i file descriptor in lettura/scrittura diretta sul socket
        if(XDR_MODE == ENABLED){
			if ( ( stream_socket_r = fdopen(conn_s, "r")) == NULL ||  ( stream_socket_w = fdopen(conn_s, "w")) == NULL ){
				printf("[debug] worker %d -> errore nell'apertura di conn_s in lettura o scrittura (FILE* stream_socket_*). Non è possibile servire l'host.\n", getpid());
				continue; //poniti nuovamente in ascolto
			}
		}

        printf("Worker %d - Host %s:%d just connected...\n", getpid(), getPrintableIP(remoteHostInfo), getPrintablePort(remoteHostInfo));
   
        /**
         * servo l'host remoto fino a quando non chiude la connessione o invia messaggio sbagliato
         */
        do{

            int res;

            /*
            * Serve the remote host
            */
            if( XDR_MODE == ENABLED)
                res = serveRemoteHostXDR(conn_s, stream_socket_r, stream_socket_w, remoteHostInfo, remoteHostInfoLen);
            else if( XDR_MODE == DISABLED)
                res = serveRemoteHost(conn_s, remoteHostInfo, remoteHostInfoLen);
	
            if(res == CLIENT_SERVED){

                //servilo nuovamente fino a quando non invia comando QUIT
                printf("Worker %d - Ready to serve again the remote host %s:%d \n", getpid(), getPrintableIP(remoteHostInfo), getPrintablePort(remoteHostInfo));
                continue;

            } else if(res == QUIT_REQUESTED || res == WRONG_COMMAND || res == ERROR_ENCOUNTERED){

                //chiudo il socket connesso e pulisco strutture dati per accettare futura connessione
                printf("Worker %d - Closing connection to remote host %s:%d \n", getpid(), getPrintableIP(remoteHostInfo), getPrintablePort(remoteHostInfo));
				
				if( XDR_MODE == ENABLED){
					
					if( res != QUIT_REQUESTED)
						sendErrMsgXDR(conn_s, stream_socket_w); //invia messaggio di errore al client
					
					Close(conn_s);               
					memset(&remoteHostInfo, 0, sizeof(remoteHostInfo));
            
				} else if( XDR_MODE == DISABLED){
					
					if(res != QUIT_REQUESTED){                    
						sendErrMsg(conn_s);
						Close(conn_s);               
						memset(&remoteHostInfo, 0, sizeof(remoteHostInfo));
					}
                }
				
				break; //poniti nuovamente in ascolto per nuove connessioni
            }
            
        } while(1);
        
    }

    exit(EXIT_FAILURE);
};


/********************
 * serveRemoteHost
 * 
 * Legge comando ricevuto dal socket (codifica binaria) ed effettua l'azione relativa
 */
int serveRemoteHost(int conn_s, struct sockaddr_in remoteHostInfo, int remoteHostInfoLen){

    char cmd[5] = ""; //buffer in cui verrà letto il comando
    
    //comandi supportati
    char cmd_get[5] = {'G', 'E', 'T', ' ', '\0'};
    char cmd_quit[5] = {'Q', 'U', 'I', 'T', '\0'};

    ssize_t nread;
    size_t nleft = 4; //we must first read the four characters "GET " or "QUIT"

    char c;
    char filename[MAX_FILENAME_LENGTH] = "";
    int pos = 0;

    //strutture per gestire select
    fd_set readSet; //insiemi di socket su cui si vuole verificare la condizione di lettura
    int sel_cond = 0; //usato per return select
    struct timeval tval;
    int time;

    /*
    * Setup della select: 
    */
    FD_ZERO(&readSet); //azzera insieme
    FD_SET(conn_s, &readSet); //inserisce s nell'insieme dei socket puntato da readSet
    time = WAIT_TIME; tval.tv_sec = time; tval.tv_usec = 0; //tempo per cui occore aspettare il verificarsi di uno degli eventi
    if( (sel_cond = select(FD_SETSIZE, &readSet, NULL, NULL, &tval )) == ERROR_ENCOUNTERED) {
        printf("[error] Worker %d - select failed.", getpid());
        return ERROR_ENCOUNTERED;
    }

    if( sel_cond > 0 ){ //verificata condizione di leggibilità sul socket, select si è sbloccata.

        /*
         * Leggi il comando, può essere: "GET ", "QUIT"
         */
        if( readNumBytes(conn_s, cmd, nleft) != nleft ) return ERROR_ENCOUNTERED; 
        cmd[4] = '\0'; //add \0 to the end of cmd so that strcmp can be used

        // Intraprendi azione richiesta
        if( strcmp(cmd, cmd_get) == 0 ) { //cmd=="GET "
            
            printf("[debug] Worker %d - Host sended %s command. getting file info...\n", getpid(), cmd_get);

            //leggo nome del file dal socket
            do {
                nread = MyRecv(conn_s, &c, 1, 0); //leggi un carattere
                if( nread == ERROR_ENCOUNTERED){
                    return ERROR_ENCOUNTERED;

                } else if(nread == 1 && c != '\r' && c != '\n'){
                    //aggiungo carattere solo se fa parte del nome del file (comunque leggo i restanti per pulire buffer)
                    filename[pos++] = c;
                }
            } while( c != '\n' );

            filename[pos] = '\0'; //aggiungo esplicitamente terminatore di stringa anche se è inizializzata a ""
            printf("WOrker %d - the requested file is: %s.\n", getpid(), filename);

            return sendFile(conn_s, remoteHostInfo, remoteHostInfoLen, filename); // INVIO FILE

        } else if (strcmp(cmd, cmd_quit) == 0){ //cmd=="QUIT"
            
            printf("Worker %d - Host sended '%s' command, closing connection.\n", getpid(), cmd_quit);
            return QUIT_REQUESTED;

        } else { //Wrong command, closing connection.

            printf("Worker %d - Host sended a wrong command: '%s', closing connection.\n", getpid(), cmd);
            return WRONG_COMMAND;
        }

    } else {

        printf("Worker %d - Host didn't sent the command in time, %d seconds elapsed.\n", getpid(), WAIT_TIME);
        return ERROR_ENCOUNTERED;
    }
};


/***************
 *	sendFile
 * 
 *	Invia il file implementando la risposta al comando "GET filename", con codifica binaria
 */
int sendFile(int conn_s, struct sockaddr_in remoteHostInfo, int remoteHostInfoLen, char* filename){

    struct stat fileInfo;
    uint32_t lastMod, size;
    size_t filesize, totread = 0, lastread;

    char respHeader[13] = {'+', 'O', 'K', '\r', '\n'}; //buffer che contiene l'header da inviare al client

    char chunk[CHUNK_SIZE];
    char pos = 5;

    FILE* fp; //filepointer del file

    /*
     * Verifico se file richiesto è presente sul disco, se non presente invio messaggio -ERR
     */
    if( (fp = fopen(filename, "r")) == 0){
        printf("Worker %d - Impossibile aprire il file %s in modalità read.\n", getpid(), filename);
        return ERROR_ENCOUNTERED;

    } else printf("Worker %d - Starting to send %s...\n", getpid(), filename);

    /*
     * Preparo header della risposta
     */

    //ricavo dimensione e tempo di ultima modifica del file
    if( stat(filename, &fileInfo) == ERROR_ENCOUNTERED ){
        return ERROR_ENCOUNTERED;
    }
    lastMod = (uint32_t)fileInfo.st_mtime; //8 BYTE integer value casted to 4
    size = (uint32_t)fileInfo.st_size; //8 BYTE integer value casted to 4
    filesize = (size_t)size;

    printf("The size and last modification time of the requested file are: %lu (%lu bytes), %lu (%lu bytes).\n",
           (unsigned long)size, sizeof(size), (unsigned long)lastMod, sizeof(lastMod));

    // conversione in NBO su 4 byte (htonl) ed inserimento nel buffer respHeader
    lastMod = htonl(lastMod);
    size = htonl(size);
    memcpy(respHeader+pos, &size, 4);
    memcpy(respHeader+pos+4, &lastMod, 4);

    //Invio il buffer respHeader
    if( MySend(conn_s, respHeader, sizeof(respHeader), 0) == ERROR_ENCOUNTERED ) {
        return ERROR_ENCOUNTERED;
    }

    /*
     * leggo file a blocchi grandi quanto CHUNK_SIZE e li invio uno per volta
     */
    while(totread != filesize ){

        //leggo un chunk del file
        lastread = readChunk(fp, chunk, CHUNK_SIZE);
        totread += lastread;
	
		//invio chunk sul socket
        if( lastread < CHUNK_SIZE ){
            //invio ultimo chunk
            if( MySend(conn_s, chunk, lastread, 0) == ERROR_ENCOUNTERED ) {
                return ERROR_ENCOUNTERED;
            }

        } else {
            //invio chunk di mezzo
            if( MySend(conn_s, chunk, CHUNK_SIZE, 0) == ERROR_ENCOUNTERED ) {
                return ERROR_ENCOUNTERED;
            }
        }
    }

    if (fclose(fp) == EOF) //chiudi filedescriptor
        return ERROR_ENCOUNTERED;

    printf("Worker %d - File correctly sended to client.\n\n", getpid());

    return CLIENT_SERVED;
}


/***********************
 * serveRemoteHostXDR
 *
 * Legge dal socket il file richiesto in modalità XDR_DECODE ed invia il file in modalità XDR_ENCODE
 */
int serveRemoteHostXDR(int conn_s, FILE* stream_socket_r, FILE* stream_socket_w, struct sockaddr_in remoteHostInfo, int remoteHostInfoLen){

    /*
    * variabili locali
    */
    int res; //Valore che verrà ritornato dalla funzione, con il risultato ottenuto dall'operazione che corrisponde al comando che verrà letto
    XDR xdrstream_r; //stream dati XDR collegato al socket in lettura, dove verranno salvati i dati letti
	
	char filename[MAX_FILENAME_LENGTH] = "";

	//buffer dati su cui vengono scritti i dati XDR letti dal socket
	message msg;
    msg.message_u.filename = NULL;
    
    //strutture per gestire select
    fd_set readSet; //insiemi di fd su cui si vuole verificare la condizione di lettura
    int sel_cond = 0; //usato per return select
    struct timeval tval;
    int time;
	
    /*
     * Setup della select:
    */
    FD_ZERO(&readSet); //azzera insieme
    FD_SET(conn_s, &readSet); //inserisce s nell'insieme dei socket puntato da readSet
    time = WAIT_TIME; tval.tv_sec = time; tval.tv_usec = 0; //tempo per cui occore aspettare il verificarsi di uno degli eventi
    if( (sel_cond = select(FD_SETSIZE, &readSet, NULL, NULL, &tval )) == ERROR_ENCOUNTERED) {
        printf("[error] select failed.");
        return ERROR_ENCOUNTERED;
    }
	

    if( sel_cond > 0 ) { //verificata condizione di leggibilità sul socket, select si è sbloccata.
        
        xdrstdio_create(&xdrstream_r, stream_socket_r, XDR_DECODE); //collega il socket connesso allo stream XDR in modo da leggere direttamente tutti i dati nello stream XDR
        
        /*
        * leggi il comando dallo stream XDR ed esegui azione richiesta
        */
        if( !xdr_message(&xdrstream_r, &msg) ){ 
			
			printf("worker %d - impossibile leggere il comando ricevuto dall'host %s:%d.\n", getpid(),
					getPrintableIP(remoteHostInfo), getPrintablePort(remoteHostInfo));
			res = ERROR_ENCOUNTERED;
		
		} else if (msg.tag == GET) { //tag == GET

            strcpy(filename, msg.message_u.filename);

            printf("worker %d - ricevuto messaggio GET dall'host %s:%d. file richiesto: %s\n", getpid(),
                   getPrintableIP(remoteHostInfo), getPrintablePort(remoteHostInfo), filename);

            res = sendFileXDR_mmap(conn_s, stream_socket_r, stream_socket_w, remoteHostInfo, remoteHostInfoLen, filename); //invio file all'host con codifica XDR

        } else if (msg.tag == QUIT) { //tag == QUIT

            printf("worker %d - ricevuto messaggio QUIT dall'host %s:%d.\n", getpid(),
                   getPrintableIP(remoteHostInfo), getPrintablePort(remoteHostInfo));
            res = QUIT_REQUESTED;

        } else { //tag non corretto
			
            printf("worker %d - ricevuto messaggio non correto dall'host %s:%d.\n", getpid(),
                   getPrintableIP(remoteHostInfo), getPrintablePort(remoteHostInfo));
            res = ERROR_ENCOUNTERED;
        }

		free(msg.message_u.filename);
		xdr_destroy(&xdrstream_r); //distruggo lo stream
		return res; //ritorno risultato dell'azione richiesta

    } else { //il client è rimasto inattivo per troppo tempo
        
        printf("Worker %d - Host %s:%d non ha inviato messaggio entro il timeout. chiusura connessione.\n", getpid(),
               getPrintableIP(remoteHostInfo), getPrintablePort(remoteHostInfo));
               
        return ERROR_ENCOUNTERED;
    }
}



/****************
 * sendFileXDR_mmap
 * 
 *	Invia il file implementando la risposta al comando "GET filename", con codifica XDR, sfruttando la lettura del file mediante mmap
 */
int sendFileXDR_mmap(int conn_s, FILE* stream_socket_r, FILE* stream_socket_w, struct sockaddr_in remoteHostInfo, int remoteHostInfoLen, char* filename){
		
    XDR xdrstream_w; //stream XDR dove verranno salvati i dati da scrivere sul socket

	message m; //buffer dati da inviare sul socket
	
    //strutture dati per informazioni sul file
    struct stat fileInfo;
    uint32_t lastMod, size = 0;
    int resto = 0; //necessario per calcolare dimensione del padding per xdr

    int fd; //file desriptor ottenuto da sys call open()
    
    // Apro file in lettura, verifico se file richiesto è presente sul disco, se non presente invio messaggio -ERR
    if( ( fd = open(filename, O_RDONLY) ) == -1 ){
        printf("Worker %d - Impossibile aprire il file %s in modalità read (sys call open()).\n", getpid(), filename);
        return ERROR_ENCOUNTERED;

    } else printf("Worker %d - Starting to send %s...[XDR]\n", getpid(), filename);
	

    /*
     * Preparo header della risposta
     */

    //ricavo dimensione e tempo di ultima modifica del file: NO conversione in NBO per XDR!
    if( stat(filename, &fileInfo) == ERROR_ENCOUNTERED ){
        return ERROR_ENCOUNTERED;
    }
    lastMod = (uint32_t)fileInfo.st_mtime; //8 BYTE integer value casted to 4
    size = (uint32_t)fileInfo.st_size; //8 BYTE integer value casted to 4

    printf("Worker %d - The size and last modification time of the requested file %s are: %lu (%lu bytes), %lu (%lu bytes).\n", getpid(), filename,
           (unsigned long)size, sizeof(size), (unsigned long)lastMod, sizeof(lastMod));
    
    /*
     * Preparo buffer XDR per inviare l'header + file mappato
     */
    xdrstdio_create(&xdrstream_w, stream_socket_w, XDR_ENCODE); //collega stream XDR al socket

    m.tag = OK; //tatype
    m.message_u.fdata.last_mod_time = (uint)lastMod; //timestamp
    m.message_u.fdata.contents.contents_len = (uint)size; //filesize (senza padding!)
    
	//file data lenght: calcola padding
	if( (resto = size % 4) != 0){
		resto = 4 - resto;
		size = size + resto; //aggiungo byte di padding, unità fondamentale di xdr è di 4 byte	
	} else resto = 0;
		
	/*
	 * v2.0: mappo il file in memoria usando mmap e lo invio (mmap lo legge allocando un buffer pari ad un multiple del page size (inizializzato a zero, no problema per padding!))
	 */
    if( (m.message_u.fdata.contents.contents_val = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED ){
		
		printf("Worker %d - errore nella mappatura del file in memoria (mmap). Interruzione invio. chiusura fd.\n", getpid());
		close(fd);
		return ERROR_ENCOUNTERED;
	
	} else printf("Worker %d - file mappato (mmap) correttamente da disco in memoria.\n", getpid());
	
	
	//invio stream xdr sul socket
	if( !xdr_message(&xdrstream_w, &m) ){
		
		printf("Worker %d - errore nell'invio della struct xdr_message. Interruzione invio. liberazione memoria e chiusura fd.", getpid());
		munmap(m.message_u.fdata.contents.contents_val, size );
		close(fd);
		return ERROR_ENCOUNTERED;
		
	} else printf("Worker %d - stream xdr caricato. flush dello stream sul socket in corso.\n", getpid());


	fflush(stream_socket_w); // i/o è bufferizzato! forzo invio dello stream

    if (close(fd) == -1)
        return ERROR_ENCOUNTERED;

	//invio effettuato correttamente, libero memoria mappata ed allocata
    xdr_destroy(&xdrstream_w);

	if( munmap(m.message_u.fdata.contents.contents_val, size ) == -1 ){
		
		printf("Worker %d - errore nell'un-map del file in memoria.\n", getpid());
		return ERROR_ENCOUNTERED;
	
	} else printf("Worker %d - eliminata mappatura del file correttamente dalla memoria.\n", getpid());
	

	printf("Worker %d - File correctly sended to client.\n\n", getpid());
	
    return CLIENT_SERVED;
}



/****************
 * sendErrMsg
 * 
 * Invia messaggio di errore all'host: -ERR
 */
void sendErrMsg(int conn_s) {

    char err_cmd[6] = {'-', 'E', 'R', 'R', '\r', '\n'};
    char* buf = err_cmd;

    ssize_t nsend;
    size_t remaining = sizeof(err_cmd);

    while( remaining != 0 ){

        nsend = MySend(conn_s, buf, remaining, 0); //invio un pezzo (magari tutto)

        if(nsend == ERROR_ENCOUNTERED){
            printf("[error] sendErrMsg failed. returning to main process.\n");
            return;
        }

        remaining -= nsend;
        buf += nsend;
    }

    printf("Worker %d - Error message sent.\n", getpid());
    return;
}





/********************
 * sendErrMsgXDR
 * 
 * Invia messaggio di errore all'host: -ERR con codifica XDR
 */
void sendErrMsgXDR(int conn_s, FILE* stream_socket_w) {

	XDR xdrstream_w; //stream dati xdr
	message m; //struct per codifica XDR

    xdrstdio_create(&xdrstream_w, stream_socket_w, XDR_ENCODE); //collego stream xdr a fd collegato in scrittura al socket connesso

	m.tag = ERR;

	if ( !xdr_message(&xdrstream_w, &m) ) { //invio comando ERR al client
		printf("[error] xdr_tagtype returned with error. Riprova.\n");
		exit(EXIT_FAILURE);
	}

	fflush(stream_socket_w); //flush dei dati sul socket	
	xdr_destroy(&xdrstream_w);	//free del buffer xdr
	
	printf("Worker %d - Error message (XDR) sent.\n", getpid());
	return;
}

