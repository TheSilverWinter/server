#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>

#define SERVER_PORT 6969      
#define PACKET_SIZE 516
#define DATA_SIZE 512
#define MAX_RETRIES 5 
#define TIMEOUT 2 

#define OP_RRQ 1 
#define OP_WRQ 2      
#define OP_DATA 3 
#define OP_ACK 4 
#define OP_ERROR 5 

// Configure un timeout de réception sur la socket
void set_timeout(int sock) {
    struct timeval timeout = {TIMEOUT, 0};  // Timeout de 2 secondes.
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

// Construit et envoie la requête initiale (WRQ ou RRQ)
void send_request(int sock, struct sockaddr_in *server_addr, const char *filename, int opcode) {
    char buffer[PACKET_SIZE];
    // Construit le paquet : 0, opcode, nom_du_fichier, 0, "octet", 0
    // Attention : ici sprintf est utilisé pour simplifier, mais dans un contexte binaire il faut veiller aux caractères NUL.
    int len = sprintf(buffer, "%c%c%s%c%s%c", 0, opcode, filename, 0, "octet", 0);
    
    printf("Envoi de la requête %s pour le fichier : %s\n",
           (opcode == OP_WRQ) ? "WRQ" : "RRQ", filename);
    
    sendto(sock, buffer, len, 0, (struct sockaddr *)server_addr, sizeof(*server_addr));
}

// Pour une requête de lecture (RRQ) : réception et écriture dans le fichier local.
void receive_file(int sock, struct sockaddr_in *server_addr, const char *filename) {
    char buffer[PACKET_SIZE];
    socklen_t addr_len = sizeof(*server_addr);
    
    // Réception du premier paquet depuis le serveur (DATA ou ERROR)
    int bytes_received = recvfrom(sock, buffer, PACKET_SIZE, 0,
                                  (struct sockaddr *)server_addr, &addr_len);
    if (bytes_received < 4) {
        perror("recvfrom (initial RRQ)");
        return;
    }
    
    // Vérifier si le paquet est une erreur (OP_ERROR)
    uint16_t opcode = (((unsigned char)buffer[0]) << 8) | ((unsigned char)buffer[1]);
    if (opcode == OP_ERROR) {
        uint16_t error_code = (((unsigned char)buffer[2]) << 8) | ((unsigned char)buffer[3]);
        fprintf(stderr, "Erreur du serveur: code %d, message: %s\n", error_code, buffer + 4);
        return;
    }
    
    // Mise à jour de la connexion : on se connecte sur la socket au port indiqué par le serveur
    if (connect(sock, (struct sockaddr *)server_addr, addr_len) < 0) {
        perror("connect (RRQ)");
        return;
    }
    printf("Connexion établie vers %s:%d\n", inet_ntoa(server_addr->sin_addr), ntohs(server_addr->sin_port));
    
    // Ouvre le fichier local en écriture
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("fopen (RRQ fichier local)");
        return;
    }
    
    int block = 1;
    do {
        uint16_t block_received = (((unsigned char)buffer[2]) << 8) | ((unsigned char)buffer[3]);
        printf("Reçu bloc %d avec %d octets\n", block_received, bytes_received - 4);
        fwrite(buffer + 4, 1, bytes_received - 4, file);
        fflush(file);
        
        // Prépare et envoie l'ACK pour le bloc reçu
        unsigned char ack[4] = {0, OP_ACK, buffer[2], buffer[3]};
        send(sock, ack, 4, 0);
        printf("Envoi de l'ACK pour le bloc %d\n", block_received);
        
        // Si la taille des données est inférieure à DATA_SIZE, c'est le dernier bloc.
        if ((bytes_received - 4) < DATA_SIZE)
            break;
        
        // Réception du bloc suivant (la socket est connectée, on utilise recv())
        bytes_received = recv(sock, buffer, PACKET_SIZE, 0);
        if (bytes_received < 0) {
            perror("recv (RRQ DATA)");
            break;
        }
        block++;
    } while (1);
    
    printf("Réception du fichier terminée.\n");
    fclose(file);
}

// Envoi du fichier pour une requête d'écriture (WRQ)
void send_data(int sock, struct sockaddr_in *server_addr, FILE *file) {
    char buffer[PACKET_SIZE];
    int block = 1, bytes_read, retries;
    char ack[PACKET_SIZE];  // élargi pour pouvoir lire un message d'erreur
    socklen_t addr_len = sizeof(*server_addr);
    
    // Boucle de réception de l'ACK initial depuis un port autre que SERVER_PORT
    retries = 0;
    while (1) {
        int ret = recvfrom(sock, ack, sizeof(ack), 0, (struct sockaddr *)server_addr, &addr_len);
        if (ret >= 4) {
            // Si c'est un paquet d'erreur, l'afficher et quitter
            if ( (unsigned char)ack[1] == OP_ERROR ) {
                fprintf(stderr, "Erreur du serveur : %s\n", ack + 4);
                return;
            }
            // Si le paquet vient du port bien connu, on l'ignore
            if (server_addr->sin_port == htons(SERVER_PORT)) {
                printf("ACK initial reçu depuis le port %d (port bien connu), ignoré...\n", ntohs(server_addr->sin_port));
                continue;
            }
            break;
        }
        if (++retries > MAX_RETRIES) {
            printf("Erreur : pas d'ACK initial reçu, annulation.\n");
            return;
        }
        printf("Attente de l'ACK initial, tentative %d/%d\n", retries, MAX_RETRIES);
    }
    printf("ACK initial reçu. Connexion établie vers %s:%d\n",
           inet_ntoa(server_addr->sin_addr), ntohs(server_addr->sin_port));
    
    // Se connecter sur la socket pour que les paquets suivants soient envoyés vers le bon port
    if (connect(sock, (struct sockaddr *)server_addr, addr_len) < 0) {
        perror("connect (WRQ)");
        return;
    }
    
    while ((bytes_read = fread(buffer + 4, 1, DATA_SIZE, file)) > 0) {
        buffer[0] = 0;
        buffer[1] = OP_DATA;
        buffer[2] = (block >> 8) & 0xFF;
        buffer[3] = block & 0xFF;

        retries = 0;
        do {
            send(sock, buffer, bytes_read + 4, 0);
            printf("Envoi du bloc %d (%d octets)\n", block, bytes_read);
            int ret = recv(sock, ack, sizeof(ack), 0);
            if (ret >= 4) {
                // Vérifier si on a reçu un paquet d'erreur
                if ((unsigned char)ack[1] == OP_ERROR) {
                    fprintf(stderr, "Erreur du serveur lors de l'envoi du bloc %d : %s\n", block, ack + 4);
                    return;
                }
                break;
            }
            printf("ACK non reçu, réessai %d/%d\n", ++retries, MAX_RETRIES);
        } while (retries < MAX_RETRIES);

        if (retries == MAX_RETRIES) {
            printf("Erreur : le bloc %d n'a pas été confirmé, annulation.\n", block);
            return;
        }
        printf("ACK reçu pour le bloc %d\n", block);
        block++;
    }
    
    printf("Transfert de fichier terminé.\n");
}


int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Utilisation : %s <IP serveur> <WRQ|RRQ> <fichier>\n", argv[0]);
        return 1;
    }
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, argv[1], &server_addr.sin_addr);
    
    set_timeout(sock);
    
    int opcode = (strcmp(argv[2], "WRQ") == 0) ? OP_WRQ : OP_RRQ;
    
    // Envoi de la requête initiale
    send_request(sock, &server_addr, argv[3], opcode);
    
    if (opcode == OP_WRQ) {
        // WRQ : envoi du fichier
        FILE *file = fopen(argv[3], "rb");
        if (!file) {
            perror("Erreur ouverture fichier");
            return 1;
        }
        send_data(sock, &server_addr, file);
        fclose(file);
    } else {
        // RRQ : réception du fichier
        receive_file(sock, &server_addr, argv[3]);
    }
    
    close(sock);
    return 0;
}

