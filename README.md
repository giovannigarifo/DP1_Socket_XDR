## Project description

Implementation of both a server and a client that can handle dual protocol communications (ASCII and XDR) via the socket API in C programming language. 

The client and the server are non blocking, they support dual stack IPv4/v6 interoperability, and the server supports concurrency.

### How to compile

* In the client directory: `gcc client.c ../utilities/*.c ../xdr_types/types.c -o client`
* In the server directory: `gcc server.c ../utilities/*.c ../xdr_types/types.c -o server`

### How to run

* First run the server: `./server [-x] <server_port>`
* and then run a client: `./client [-x] <server_ip> <server_port>`

`-x` option is used to trigger XDR mode. Don't specify any option to run in ASCII mode.
