#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/time.h>

#define TFTP_PORT 69        // Port utilisé
#define PACKET_SIZE 516     // Taille max d'un package
#define DATA_SIZE 512       // Taille max du paquet de données
#define OP_RRQ 1            // OP code demande lecture
#define OP_WRQ 2            // OP code demande écriture
#define OP_DATA 3           // OP code paquet de données
#define OP_ACK 4            // Signal attendu
#define OP_ERROR 5          // Signal d'erreur
#define MAX_RETRIES 5       // Nombre maximal de tentatives de retransmission
#define TIMEOUT 2           // Timeout en secondes

/* Variables utilisées :
- sock = socket
- buffer[] = buffer dans lequel on stocke 4 bytes + 512 bytes de données
- ack[] = réponse du serveur
- block = numéro de bloc de données
- opcode = 1 ou 2 (RRQ ou WRQ)
- filename = fichier à manipuler (lire/écrire)
- len = longueur du packet de données
- server_addr = connexion réseau
- addr_len = taille de (la structure) server_addr
*/

// Fonction pour configurer le timeout
void set_timeout(int sock) {
    struct timeval timeout = {TIMEOUT, 0};  // Timeout de 2 secondes
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

// Fonction pour envoyer la requête au serveur
void send_request(int sock, struct sockaddr_in *server_addr, const char *filename, int opcode) {
    char buffer[PACKET_SIZE];
    int len = sprintf(buffer, "%c%c%s%c%s%c", 0, opcode, filename, 0, "octet", 0);
	printf("Requête envoyée: %s\n", buffer);  // Afficher la requête envoyée

    if(opcode == OP_WRQ){
        printf("Requête WRQ pour le fichier: %s\n", filename);
    } else {
        printf("Requête RRQ pour le fichier: %s\n", filename);
    }
    
    sendto(sock, buffer, len, 0, (struct sockaddr *)server_addr, sizeof(*server_addr));  // Envoi de la requête au serveur
}

// Fonction pour envoyer les données (WRQ)
void send_data(int sock, struct sockaddr_in *server_addr, FILE *file) {
    char buffer[PACKET_SIZE];
    int block = 1, bytes_read, addr_len = sizeof(*server_addr);
    char ack[4];
    int retries;

    // Attente de l'ACK initial du serveur
    while (recvfrom(sock, ack, 4, 0, (struct sockaddr *)server_addr, &addr_len) < 4) {
        printf("Erreur, ACK non reçus\n");
    }

    if (ack[1] != OP_ACK || ack[2] != 0 || ack[3] != 0) {  // Vérifie la réponse du serveur
        printf("Erreur de la réponse du serveur\n");
        return;
    }
    printf("ACK reçus\n");

    // Envoi des blocs de données
    while ((bytes_read = fread(buffer + 4, 1, DATA_SIZE, file)) > 0) {
        buffer[0] = 0;
        buffer[1] = OP_DATA;
        buffer[2] = (block >> 8) & 0xFF;  // Numéro du bloc (octet gauche)
        buffer[3] = block & 0xFF;         // Numéro du bloc (octet droit)

        printf("Sending block %d with %d bytes\n", block, bytes_read);

        retries = 0;
        // Tentatives de retransmission en cas de perte de paquet
        while (retries < MAX_RETRIES) {
            sendto(sock, buffer, bytes_read + 4, 0, (struct sockaddr *)server_addr, addr_len);  // Envoi des données
            printf("Bloc %d envoyé, attente du ACK\n", block);
            
            // Attente de l'ACK du serveur avec timeout
            if (recvfrom(sock, ack, 4, 0, (struct sockaddr *)server_addr, &addr_len) >= 4) {
                // Si ACK reçu, on sort de la boucle de retransmission
                if (ack[1] == OP_ACK && ack[2] == (block >> 8) && ack[3] == (block & 0xFF)) {
                    printf("ACK reçus pour le bloc %d\n", block);
                    break;
                }
            } else {
                // Si pas d'ACK reçu, on réessaie
                printf("ACK non reçus (%d/%d)\n", ++retries, MAX_RETRIES);
            }
        }

        // Si on dépasse le nombre de tentatives, on annule
        if (retries == MAX_RETRIES) {
            printf("ACK non reçus pour le bloc %d arpès %d essais\n", block, MAX_RETRIES);
            return;
        }

        block++;  // On passe au bloc suivant
    }

    printf("Transfer finis\n");
}

// Fonction pour recevoir le fichier (RRQ)
void receive_file(int sock, struct sockaddr_in *server_addr, const char *filename) {
    FILE *file = fopen(filename, "wb");  // Ouverture du fichier à écrire
    char buffer[PACKET_SIZE], ack[4] = {0, OP_ACK, 0, 0};
    int bytes_received, addr_len = sizeof(*server_addr), block = 1;
    int retries;

    // Réception des blocs de données
    do {
        retries = 0;
        while ((bytes_received = recvfrom(sock, buffer, PACKET_SIZE, 0, (struct sockaddr *)server_addr, &addr_len)) < 4) {
            // Si pas assez de données reçues, on réessaie
            if (++retries > MAX_RETRIES) {
                printf("Packet perdu.\n");
                fclose(file);
                return;
            }
            printf("Packet Perdu, nouvelle tentative (%d/%d)\n", retries, MAX_RETRIES);
        }

        printf("Bloc %d reçus (%d bytes)\n", block, bytes_received - 4);
        fwrite(buffer + 4, 1, bytes_received - 4, file);  // Ecriture des données dans le fichier
        
        ack[2] = buffer[2];
        ack[3] = buffer[3];
        
        // Envoi de l'ACK pour le bloc reçu
        sendto(sock, ack, 4, 0, (struct sockaddr *)server_addr, addr_len);
        printf("ACK envoyé pour le bloc %d\n", block);
        
        block++;  // On passe au bloc suivant
    } while (bytes_received == PACKET_SIZE);  // Si le paquet était complet, on attend le bloc suivant

    printf("Fin de la reception.\n");
    fclose(file);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <server_ip> <WRQ|RRQ> <filename>\n", argv[0]);
        return 1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;  // Définition IPv4
    server_addr.sin_port = htons(TFTP_PORT);  // Définition du Port
    inet_pton(AF_INET, argv[1], &server_addr.sin_addr);  // Définition de l'adresse IP

    printf("Connection au server TFTP %s\n", argv[1]);
    int opcode = 1;
    if (strcmp(argv[2], "WRQ") == 0) opcode = OP_WRQ;

    set_timeout(sock);  // Activation du timeout pour la socket

    send_request(sock, &server_addr, argv[3], opcode);  // Demande au serveur pour lire/écrire

    if (opcode == OP_WRQ) {  // Si WRQ, on envoie les données
        FILE *file = fopen(argv[3], "rb");
        if (!file) {
            perror("Fichier");
            return 1;
        }
        send_data(sock, &server_addr, file);
        fclose(file);
    } else {  // Si RRQ, on reçoit les données
        receive_file(sock, &server_addr, argv[3]);
    }

    printf("TFTP Fini.\n");
    close(sock);
    return 0;
}
