/**
* data structures
**/

struct pico_mqtt_socket{
	int descriptor;
};

/**
* private functions prototypes
**/

static struct pico_mqtt_socket* connection_open( const char* URI );
static const struct addrinfo* resolve_URI( const char* URI );
static const int socket_connect( const struct addrinfo* addresses );
static const struct addrinfo lookup_configuration( void );

/**
* public functions implementations
**/

// create pico mqtt socket and connect to the URI, returns NULL on error
struct pico_mqtt_socket* pico_mqtt_connection_open( const char* URI, uint32_t timeout){

}

// read data from the socket, return the number of bytes read
int pico_mqtt_connection_read( struct pico_mqtt_socket* socket, void* read_buffer, uint16_t count, uint32_t timeout);

// write data to the socket, return the number of bytes written
int pico_mqtt_connection_write( struct pico_mqtt_socket* socket, void* write_buffer, uint16_t count, uint32_t timeout);

// close pico mqtt socket, return -1 on error
int pico_mqtt_connection_close( int socket_file_descriptor );

/**
* private function implementation
**/

static struct pico_mqtt_socket* connection_open( const char* URI ){
	struct addrinfo* addres = resolve_URI( URI );
	if(addres == NULL){
		return 0;
	}

	struct pico_mqtt_socket* socket = (struct pico_mqtt_socket*) malloc(sizeof(struct pico_mqtt_socket));
	socket.descriptor = socket_connect (addres);

	if(socket.descriptor == -1){
		free(socket);
		return NULL;
	}

	return socket;
}

// return the socket file descriptor
static const int socket_connect( const struct addrinfo* addres ){
	struct addrinfo* current_addres;
	int socket_descriptor;
	int result = 0;

	for (current_addres = addres; current_addres != NULL; current_addres = current_addres->ai_next) {
		socket_descriptor = socket(current_addres->ai_family, current_addres->ai_socktype, current_addres->ai_protocol);
		
		if (socket_descriptor == -1)
			continue;
		
		result = connect(socket_descriptor, current_addres->ai_addr, current_addres->ai_addrlen);
		if ( result != -1)
			return socket_descriptor; //succes
		
		close(socket_descriptor);
	}
	
	return -1; //failed
}

// return a list of IPv6 addresses
static struct addrinfo* resolve_URI( const char* URI ){
	struct addrinfo * res;
	const struct addrinfo hints =  lookup_configurations();

	//TODO specify the service, no hardcoded ports.
	int result = getaddrinfo( URI, "1883", &hints, &res );
	if(result != 0){
		return NULL;
	}
	
	return res;
}

// return the configuration for the IP lookup
static const struct addrinfo lookup_configuration( void ){
	struct addrinfo hints;
	int* flags = &(hints.ai_flags);


	// configure the hints struct
	hints.ai_family = AF_INET6; // allow only IPv6 address, IPv4 will be mapped to IPv6
	hints.ai_socktype = SOCK_STREAM; // only use TCP sockets
	hints.ai_protocol = 0; //allow any protocol //TODO check for restrictions
	
	// clear the flags before setting them
	flags = 0;

	// set the appropriate flags
#if (PICO_MQTT_DNS_LOOKUP == 1)
	flags |= AI_NUMERICHOST;
#endif /*(PICO_MQTT_DNS_LOOKUP = 1)*/
	// AI_PASSIVE unsed, intend to use address for a connect
	// AI_NUMERICSERV unsed, //TODO check for this restriction
#if (PICO_MQTT_HOSTNAME_LOOKUP ==1)
	flags |= IA_CANONNAME;
#endif /*(PICO_MQTT_HOSTNAME_LOOKUP ==1)*/
	//AI_ADDRCONFIG unsed, intedded to use IPv6 of mapped IPv4 addresses
	flags |= AI_V4MAPPED; // map IPv4 addresses to IPv6 addresses
	flags |= AI_ALL; // return both IPv4 and IPv6 addresses

		

	// set the unused variables to 0
	hints.ai_addrlen = 0;
	hints.ai_addr = NULL;
	hints.ai_canonname = NULL;
	hints.ai_next = NULL;

	return hints;
}
#endif
