/************************************************************************************
*
*   		3.4 - Client TCP che richiede file a Server TCP Iterativo
*
*	Codifica ASCII o XDR
*
*************************************************************************************/

/*
*	Includes 
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/stat.h>


#include "../utilities/myutilities.h" //mia libreria che contiene funzioni di utilità
#include "../utilities/sockwrap.h" //wrappers with error handling for socket functions

#include "../xdr_types/types.h"

/*
*	Constants
*/
#define NOT_VALID 0
#define ERROR_ENCOUNTERED -1
#define MAX_FILENAME_LENGTH 30
#define MAX_FILE_NUMBER 5
#define WAIT_TIME 5
#define MAX_USER_INPUT 25
#define BUSY 0 //client in stato busy se è in attesa di completare la ricezione di un file
#define FREE 1 //client in stato free se non è in attesa di completare ricezione, deve ancora richiedere file
#define QUIT_REQUESTED 2
#define ABORT_REQUESTED 3
#define ENABLED 1
#define DISABLED 0

//valori ritornati da readHeader_A
#define CMD_OK 10
#define CMD_ERR 11



/************************
* 		Prototypes
*************************/
int testCmdGET(char* user_input, char filename[]); //effettua test sul comando inserito dall'utente

void sendCmdGET_A( int s, char* filename); //crea la stringa con il comando GET e la invia sul socket
void sendCmdGET_X( char* filename, FILE* stream_socket_w); //crea buffer XDR con comando GET e lo invia sul socket

void sendCmdQUIT_A( int s );
void sendCmdQUIT_X( FILE* stream_socket_w );

int readHeader_A(int s, char* filename, int* filesize); //attende ricezione del file (select) e lo salva.

int readFile_X( FILE* fp, char* filename, int* filesize, FILE* stream_socket_r ); //funzione chiamata per effettuare la lettura del file da socket secondo il protocollo ASCII

/************************
*	Global Variables
*************************/
char* prog_name;
int status = FREE; //stato del client
int XDR_MODE = DISABLED; //modalità di codifica/decodifica, letta da argv
ssize_t totread = 0; //numero di byte letti fino ad un certo punto per il traferimento di un file


/**********************************
 *  			Main
 **********************************/
 int main(int argc, char** argv)
{
    //strutture dati
    int s = 0; //file descriptor del socket
    
    char filename[MAX_FILENAME_LENGTH] = "";
    int filesize = 0;
    FILE* fp = NULL; //file pointer del file da creare
    char chunk[CHUNK_SIZE] = ""; //blocco di dati letti/scritti

    char user_input[MAX_USER_INPUT] = ""; //buffer dove viene salvato l'input preso da fgets

    //user commands
    char cmd_Q = 'Q';
    char cmd_A = 'A';
    
    //response received from server
    int cmd_response = 0;

    //variabili necessarie per gestire XDR
    int argc_offset = 0;

    FILE* stream_socket_r = NULL; //stream io direttamente collegati al socket
    FILE* stream_socket_w = NULL;
    

    /*********************************
    *   (1) Convalida Input
    **********************************
    *   - argv[0] nome del programma
    *   - argv[1] -> OPZIONALE: -x se abilitato XDR
    *   - argv[2] -> IP del server a cui collegarsi
    *   - argv[3] -> porta su cui il server è in ascolto
    */
    if( argc <3 || argc>4 ){

        printf("[error] numero dei parametri errato, sintassi corretta: $ %s [-x] ip_server binding_port\n\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    //calcolo offset degli argomenti se presente il parametro -x
    if(argc == 3){
		
        argc_offset = 0;
		XDR_MODE = DISABLED;
        
    } else if(argc == 4){
        argc_offset = 1;

        //test su parametro opzionale
        if( strlen(argv[1]) == 2 && argv[1][0] == '-' && argv[1][1] == 'x' )
            printf("abilitata funzionalità XDR. i dati verranno scambiati secondo la codifica XDR.\n");
        else {
            printf("Errore nel comando -x\n");
            exit(EXIT_FAILURE);
        }

        XDR_MODE = ENABLED;
    }

	//commentato per permettere risoluzione hostname da parte di getaddrinfo
    /* if( testIpAddress(argv[1 + argc_offset]) == NOT_VALID  ) {

        printf("[error] ip inserito invalido: %s, formato corretto: xyz.xyz.xyz \n", argv[1]);
        exit(EXIT_FAILURE);

    } else */ if ( testPort( atoi(argv[2 + argc_offset])) == NOT_VALID ) {

        printf("[error] porta inserita non valida: %s, intervallo utilizzabile: 1025-65535\n", argv[2]);
        exit(EXIT_FAILURE);
    }

    prog_name = argv[0];
    
    /*** 
     * registring signal handlers
     */
    signal(SIGPIPE, signal_sigpipe_handler); //registro handler

    /************************
    * Creazione del Socket
    */
    s = connectToHost(argv[1 + argc_offset], argv[2 + argc_offset], TCP); //passo indirizzo ip (oppure hostname) e porta 

    // associo stream STDIO al socket, tutti i dati scritti in questo stream vengono AUTOMATICAMENTE spediti sul socket
    if( XDR_MODE == ENABLED ){
		stream_socket_r = fdopen(s, "r");
		stream_socket_w = fdopen(s, "w");
	}
	
    /******************************************************
    *  Loop Principale
    */
    printf("\nInserisci uno dei seguenti comandi:\n- GET filename\n- Q\n- A\n\n");
    
    while(1) {
		
        /*
        * Setup della Select per i/o multiplexing
        */
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(s, &readSet); //monitoro condizione leggibilità del socket
        FD_SET(0, &readSet); //e dello standard input (stdin)

        if (select(FD_SETSIZE, &readSet, NULL, NULL, NULL) == ERROR_ENCOUNTERED)
            fprintf(stderr, "[error] select failed.\n");

        /*
        * Verifico quale FD si è sbloccato ed effettuo azione, do sempre precedenza prima all'input
        */
        if (FD_ISSET(0, &readSet)) {
			/*******************
			* STDIN LEGGIBILE: leggere input utente, gli viene sempre data precedenza. CTRL+D termina programma
			*/
            if ( fgets(user_input, sizeof(user_input), stdin) != NULL ) { // reads something like: > GET nome\n\0\0\0...<

                if ( user_input[0] == cmd_Q && strlen(user_input) == 2 ) {
                    /*
                    * (1) user input: command "Q" -> close connection with QUIT after last transfer has been completed.
                    */
                    status = QUIT_REQUESTED;
                    if (filesize == 0) break; //se ancora non è iniziata ricezione file, comportamento uguale ad Abort

                } else if ( user_input[0] == cmd_A && strlen(user_input) == 2 ) {
                    /*
                    * (2) user input: command "A" -> Abort, immediatly close connection to server.
                    */
                    printf("Hai richiesto l'abort di ogni operazione e la chiusura del programma. uscita in corso...\n\n");
                    break;

                } else if ( testCmdGET(user_input, filename) == VALID ) {
                    
                    /*
                    * (3) user input: command "GET filename" riconosciuto, invia comando al server
                    */
                    if( XDR_MODE == DISABLED){
                    
                        sendCmdGET_A(s, filename); //creo stringa comando GET ed invio sul socket
					
					} else if(XDR_MODE == ENABLED){
                      
                        sendCmdGET_X(filename, stream_socket_w); //creo buffer XDR con comando GET e lo invio sul socket           
					}
					
					/*****
					 * INSERIRE QUI in un nuovo blocco else-if eventuali nuovi comandi inseriti dall'utente 
					 ****/
					
                } else {
                    /*
                    * (4) Wrong user input!
                    */
                    printf("\nErrore nel formato del comando:\n\nInserisci uno dei seguenti comandi:\n- GET filename\n- Q\n- A\n\n");
                }
             
             } else { 
				/*
				 * 5) CTRL+D (fgets ritorna NULL) termina il programma 
				 */
				printf("[debug] inserito ctrl+d, termina il programma...uscita in corso.\n\n");
				break;
			 }
            
        } else if (FD_ISSET(s, &readSet)) {
			/*******************
			* SOCKET LEGGIBILE: leggere dal socket un chunk di file
			*/
			if( XDR_MODE == DISABLED ){
				
				/*
				* Effettua lettura dei dati dal socket secondo protocollo ASCII
				*/
				if (status == FREE) {
	
					//leggo header per identificare tipo messaggio
                    cmd_response = readHeader_A(s, filename, &filesize);
                    
                    if( cmd_response == CMD_OK){
						//file presente sul server, comincio la ricezione
						status = BUSY;
						totread = 0;
						
						//reset file se già esistente sul disco
						if (fclose(fopen(filename, "w")) != 0)
							printf("Impossibile creare il file %s.\n", filename);

						//apertura file in modalità append
						if ((fp = fopen(filename, "a")) == 0)
							printf("Impossibile aprire il file %s in modalità append.\n", filename);
					
					} else if ( cmd_response == CMD_ERR ){
					
						//file non presente sul server oppure troppo tempo di inattività
						break;
					}
	
				} else if ( cmd_response == CMD_OK && (status == BUSY || status == QUIT_REQUESTED) ) {
					
					//chiusura richiesta ancora prima di richiedere il primo file
					if (status == QUIT_REQUESTED && filesize == 0) break;

					/*
					* Leggo un chunk, dopo averlo letto verifico se ho terminato o no: eventualmente chiudo il file e notifico ricezione.
					*/              
					if ( readChunkAndAppend(s, fp, chunk, filesize, &totread) == ERROR_ENCOUNTERED) {
						printf("[error] readChunkAndAppend returned with error: unable to read all the necessary byte.\n");
						break;
					}

					//test fine ricezione: ricevuto tutto il file?
					if (totread == filesize) {
						fclose(fp); 
						fp = NULL;
						totread = 0;
						filesize = 0;
						printf("File %s letto correttamente da socket e scritto correttamente su disco.\n\n", filename);

						//verifico ed aggiorno stato
						if (status == QUIT_REQUESTED)
							break; //utente ha richiesto di chiudere la comunicazione dopo aver terminato l'ultima la ricezione
						else
							status = FREE; //pongo stato a free, utente può richiedere nuovo file
							
					}
				}
		
			} else if ( XDR_MODE == ENABLED ){
				/*
				* Effettua lettura dei dati dal socket secondo protocollo XDR
				*/
				
				if (readFile_X(fp, filename, &filesize, stream_socket_r) == ERROR_ENCOUNTERED ){ //leggi l'intero file da socket, salvalo su disco e chiudilo.
					status = ERROR_ENCOUNTERED; //setto a questo valore per non inviare successivamente cmd QUIT
					break; //non sono riuscito a salvare il file
				}
					
				printf("File %s letto correttamente da socket e scritto correttamente su disco.\n\n\n", filename);
				filesize = 0;
				
			}
		}
 
    //fine ciclo while
    }
   
    /*
     * Arrivo qua solo se avviene break per via di comando Q (QUIT_REQUESTED) o errore durante readChunkAndAppend
     */
   
	if( XDR_MODE == DISABLED){
		
		//invio comando QUIT al server
		sendCmdQUIT_A(s);

	} else if (XDR_MODE == ENABLED && status != ERROR_ENCOUNTERED){
		
		//invio comando QUIT al server
		sendCmdQUIT_X(stream_socket_w);	
	}
   
    
    /******************************************************
    *   End of program: close socket and free resources
    */

    //close connection.
    Close(s);
    
    if( fp!= NULL ){
		//ricezione file interrota da abort, chiudo fd ed elimino file
		fclose(fp);
		remove(filename);
	}
	
    printf("\nCorrectly Exiting. bye.\n\n\n");
    fflush(stdout);

    return EXIT_SUCCESS;
}



/********************************************************************************
*							F U N C T I O N S									*
*********************************************************************************/


int testCmdGET(char* user_input, char filename[]){

    char cmd[4];
    char bad_chars[] = "!@%^*~|";
    char cmd_GET[4] = "GET";
    int correct_filename = TRUE;

    if( sscanf(user_input, "%s %s", cmd, filename) != 2 ){
        printf("[debug] error in testCmdGET: sscanf unable to parse user_input \n");
        return NOT_VALID;
    }

    //check su correttezza comando GET
    if(strcmp(cmd_GET, cmd) != 0){
        printf("[debug] error in testCmdGET: comando GET non formattato bene\n");
        return NOT_VALID;
    }

    //check su correttezza del nome del file
    for (int i = 0; i < strlen(bad_chars); i++) {
        if (strchr(filename, bad_chars[i]) != NULL) { //verifica se presente almeno una occorrenza del bad char
            correct_filename = FALSE;
            break;
        }
    }

    if(correct_filename == FALSE){
        printf("Filename inserito non valido: %s.", filename);
        return NOT_VALID;
    }

    return VALID;
};


/*************************************************************************************
 *	Crea stringa ASCII contenente il comando "GET filename\r\n" e la invia al server
 *  
 */
void sendCmdGET_A( int s, char* filename) {
	
	char get_prefix[4] = {'G', 'E', 'T', ' '};
	char get_suffix[2] = { '\r', '\n'};
	
    size_t len = strlen(filename); //mi ritorna il numero di caratteri non tenendo conto di eventuali \0, NB: non ci sono \n e \r per pulitura precedente

    char* cmd = malloc( sizeof(char) * (4 + len + 2) ); //"GET " + lunghezza stringa + "\r\n"

    if(cmd == NULL){
        printf("[error] error in malloc, in sendCmdGET_A.\n");
        exit(EXIT_FAILURE);
    }
	
	//concatenazione elementi per formare comando da inviare
   
    memset(cmd, 0, 4+len+2); //initialise buffer to all zeroes
    memcpy(cmd, get_prefix, 4); //copy "GET "
    memcpy(cmd+4, filename, len); //copy filename
    memcpy(cmd+4+len, get_suffix, 2); //copy "\r\n"
    
    //invio comando sul socket tramite protocollo ASCII
    if (MySend(s, cmd, len + 4 + 2, 0) == ERROR_ENCOUNTERED) //invio comando GET al server
		printf("[debug] si è verificato un errore durante l'invio del comando GET per il file %s. riprova.\n", filename);
	
	free(cmd); //free buffer from heap memory
	
    return;
}

/*************************************************************************************
 *	Crea buffer XDR contenente il comando "GET filename\r\n" e la invia al server
 *  
 */
void sendCmdGET_X( char* filename, FILE* stream_socket_w) {
	
	//definizione variabili
	XDR xdrstream_w;
	message msg; //struct per codifica XDR

	
	//collego stream xdr a fd collegato in scrittura al socket connesso
    xdrstdio_create(&xdrstream_w, stream_socket_w, XDR_ENCODE); 

	msg.tag = GET;
	msg.message_u.filename = filename;

	if (xdr_message(&xdrstream_w, &msg) == 0) { //invio richiesta GET sullo stream associato al socket
		printf("[error] xdr_message returned with error. Riprova.\n");
		exit(EXIT_FAILURE);
	}

	fflush(stream_socket_w); //flush dei dati sul socket
	
	//free del buffer xdr
	xdr_destroy(&xdrstream_w);
	
	return;
};



/*************************************************************************************
 *	Crea stringa ASCII contenente il comando "QUIT\r\n" e la invia al server
 *  
 */
void sendCmdQUIT_A( int s ) {
	
	char cmd_quit[] = {'Q', 'U', 'I', 'T', '\r', '\n'};

    if( MySend(s, cmd_quit, sizeof(cmd_quit), 0) == ERROR_ENCOUNTERED )
        printf("si è verificato un errore durante l'invio del comando QUIT al server.\n");

    return;
}

/*************************************************************************************
 *	Crea buffer XDR contenente il comando "QUIT\r\n" e la invia al server
 *  
 */
void sendCmdQUIT_X( FILE* stream_socket_w ) {
	
	//definizione variabili
	XDR xdrstream_w;
	tagtype t; //struct per codifica XDR
	
	//collego stream xdr a fd collegato in scrittura al socket connesso
    xdrstdio_create(&xdrstream_w, stream_socket_w, XDR_ENCODE); 

	t = QUIT;

	if (xdr_tagtype(&xdrstream_w, &t) == 0) { //invio richiesta QUIT sullo stream associato al socket
		printf("[error] xdr_tagtype returned with error. Riprova.\n");
		exit(EXIT_FAILURE);
	}

	fflush(stream_socket_w); //flush dei dati sul socket
	
	//free del buffer xdr
	xdr_destroy(&xdrstream_w);
	
	return;
};


/**********************************************************************************************************************
 * Legge header della risposta e ritorna il comando che è stato ricevuto
 */
int readHeader_A( int s,  char* filename, int* filesize ){

    // Strutture dati
    char cmd_ok[6] = {'+','O','K','\r','\n', '\0'};
    char cmd_err[7] = {'-', 'E', 'R', 'R', '\r', '\n', '\0'};
    char cmd_response[6] = "";
    char cmd_type;
    uint32_t last_mod_time = 0;

    printf("\n\n-> Ricevuto messaggio dal server: Lettura dell'header.\n");

    /*
    * Leggo header (lunghezza fissa) per conoscere numero di byte da leggere del file
    */

    //leggo primo carattere:
    readNumBytes(s, &cmd_type, 1); //leggo solo primo byte, per vedere se è "+" o "-" e mi comporto di conseguenza

    if(cmd_type == '+'){

            //+OK\r\n <=> 5 chars , one has already been read ^
            cmd_response[0] = '+';
            readNumBytes(s, (void*)(cmd_response+1), 4);
            cmd_response[5] = '\0';

    } else if(cmd_type == '-'){

            //-ERR\r\n <=> 6 chars, one has already been read ^
            cmd_response[0] = '-';
            readNumBytes(s, (void*)(cmd_response+1), 5);
            cmd_response[6] = '\0';
    }


    /*
     * Verifico il comando rievuto 
     */
    if(strcmp(cmd_response, cmd_err) == 0){

        printf("ricevuto comando -ERR dal server. \n");
        return CMD_ERR; // interrompi ricezione per questo file, ritorna e passa al successivo


    } else if (strcmp(cmd_response, cmd_ok) == 0){
		
		printf("letto comando +OK. si procede a leggere dimensione del file e tempo da ultima modifica: \n");
        
        //leggo i 4 byte contenenti la dimensione del file
		readNumBytes(s, (void*)filesize,  4); // 4 <=> B1 B2 B3 B4
		*filesize = ntohl(*filesize); //converto da big endian a little endian
		printf("-> file dimension: %lu. ", (unsigned long)( *filesize ));

		//leggo i 4 byte contenenti il tempo dall'ultima modifica
		readNumBytes(s, (void*)&last_mod_time, 4); // 4 <=> T1 T2 T3 T4
		last_mod_time = ntohl(last_mod_time); //conversione da big endian a little endian
		printf("-> last modified time: %lu.\n\n", (unsigned long)( last_mod_time ));

		return CMD_OK;

    }else{
		
		printf("[error] wrong command received from server. exiting for security reasons.\n");
        exit(EXIT_FAILURE);            
    }

}


/*
 * Funzione che legge il messaggio dal socket e riceve il file
 *
 */
int readFile_X( FILE* fp, char* filename, int* filesize, FILE* stream_socket_r ) {

	//strutture dati
	XDR xdrstream_r;
	message m;
	uint32_t last_mod_time; 
	
	//inizializzo a NULL puntatore al buffer che conterrà il file
	m.message_u.fdata.contents.contents_val = NULL;

	//collego stream xdr al socket in lettura
	xdrstdio_create(&xdrstream_r, stream_socket_r, XDR_DECODE);
	
	//leggo dal socket nella struttura file
	if( !xdr_message(&xdrstream_r, &m) ){
		printf("[error] errore nella lettura del file, forse il server è morto.\n");
		return ERROR_ENCOUNTERED;
	}
	
	/*
	 * Se il messaggio ricevuto NON è OK, ritorno con errore 
	 */
	if( m.tag != OK ){
		
		printf("Ricevuto messagio con tag diverso da OK dal server, file non presente. uscita. \n");
		return ERROR_ENCOUNTERED;
	}
	
	*filesize = (uint32_t) m.message_u.fdata.contents.contents_len; //converto da big endian a little endian
	last_mod_time = (uint) m.message_u.fdata.last_mod_time;
		
	printf("Lunghezza del file ricevuto: %d\n", *filesize);
	printf("Ultimo tempo di modifica: %d\n", last_mod_time);

		
	//reset file se già esistente
	if (fclose(fopen(filename, "w")) != 0)
		printf("Impossibile creare il file %s.\n", filename);

	//apertura file in modalità write
	if ((fp = fopen(filename, "w")) == 0)
		printf("Impossibile aprire il file %s in modalità scrittura.\n", filename);
            
	//scrivo file su disco
	writeChunk(fp, m.message_u.fdata.contents.contents_val, *filesize);
					
	//libero risorse usate dalla funzione
	xdr_destroy(&xdrstream_r);
	free(m.message_u.fdata.contents.contents_val);
	
	
	if (fclose(fp) != 0){
		
		printf("Impossibile chiudere il filedescriptor del file %s.\n", filename);
		return ERROR_ENCOUNTERED;
	}
	
	return *filesize;
}

