#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define SERVER_PORT 6969
#define PACKET_SIZE 516
#define DATA_SIZE 512
#define MODE "octet"
#define SERVER_FOLDER "serverFolder/"

#define OP_RRQ 1
#define OP_WRQ 2
#define OP_DATA 3
#define OP_ACK 4
#define OP_ERROR 5

#define TIMEOUT 2
#define MAX_RETRIES 5

void handle_rrq(int sock, struct sockaddr_in *client, char *filename);
void handle_wrq(int sock, struct sockaddr_in *client, char *filename);
void send_error(int sock, struct sockaddr_in *client, int code, char *msg);

int main() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);      // Création du socket
    struct sockaddr_in server_addr = {0}, client_addr;  // Structure pour stocker l'adresse du serveur.
    socklen_t addr_len = sizeof(client_addr);
    char buffer[PACKET_SIZE];
    struct timeval timeout = {TIMEOUT, 0};

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    server_addr.sin_family = AF_INET;           // IPv4
    server_addr.sin_port = htons(SERVER_PORT);  // Port
    server_addr.sin_addr.s_addr = INADDR_ANY;   // Adresse IP

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Échec du bind");
        exit(1);
    }
    
    mkdir(SERVER_FOLDER, 0777);     // Création du dossier pour stocker les fichiers
    
    printf("Serveur TFTP en écoute sur le port %d...\n", SERVER_PORT);
    
    while (1) {
        int len = recvfrom(sock, buffer, PACKET_SIZE, 0, (struct sockaddr *)&client_addr, &addr_len); // Reception de la requête
        if (len < 4) continue;
        
        int opcode = ntohs(*(short *)buffer);   // WRQ ou RRQ
        char *filename = buffer + 2;

        if (opcode == OP_RRQ) {
            printf("Demande de lecture du fichier : %s\n", filename);
            handle_rrq(sock, &client_addr, filename);   // Lecture
        } else if (opcode == OP_WRQ) {
            printf("Demande d'écriture du fichier: %s\n", filename);
            handle_wrq(sock, &client_addr, filename);   // Ecriture
        }
    }
    close(sock);
    return 0;
}

void handle_rrq(int sock, struct sockaddr_in *client, char *filename) {
    char path[256];
    sprintf(path, "%s%s", SERVER_FOLDER, filename); // Chemin du fichier
    
    int file = open(path, O_RDONLY);
    if (file < 0) {
        send_error(sock, client, 1, "Fichier introuvable");
        return;
    }
    
    char buffer[PACKET_SIZE], ack[4];
    short block = 1;
    int len;
    socklen_t addr_len = sizeof(*client);

    while ((len = read(file, buffer + 4, DATA_SIZE)) > 0) {
        *(short *)buffer = htons(OP_DATA);      // OP_Data
        *(short *)(buffer + 2) = htons(block);  // nb Bloc
        
        int retries = 0;
        while (retries < MAX_RETRIES) {
            printf("Envoi du bloc %d (%d octets)\n", block, len);
            sendto(sock, buffer, len + 4, 0, (struct sockaddr *)client, addr_len);      // Signal pour recevoir un packet
            
            if (recvfrom(sock, ack, 4, 0, (struct sockaddr *)client, &addr_len) < 0) {  // Si erreur de receptio
                if (errno == EWOULDBLOCK || errno == EAGAIN) {      // Si timeout
                    printf("Timeout. Bloc %d (%d/%d)\n", block, retries + 1, MAX_RETRIES);  
                    retries++;
                    continue;
                } else {    // Si autre source d'erreur
                    perror("recvfrom");
                    close(file);
                    return;
                }
            }
            break;
        }
        if (retries == MAX_RETRIES) {   // Si plus de tentative possibles
            printf("Abandon après %d tentatives.\n", MAX_RETRIES);
            close(file);
            return;
        }
        block++;    // Bloc suivant
    }
    printf("[INFO] Envoi terminé.\n");
    close(file);
}

void handle_wrq(int sock, struct sockaddr_in *client, char *filename) {
    char path[256];
    sprintf(path, "%s%s", SERVER_FOLDER, filename);
    
    int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (file < 0) {
        send_error(sock, client, 2, "Impossible de créer le fichier");
        return;
    }
    
    char buffer[PACKET_SIZE];
    short block = 0;
    socklen_t addr_len = sizeof(*client);
    
    *(short *)buffer = htons(OP_ACK);
    *(short *)(buffer + 2) = htons(block);
    sendto(sock, buffer, 4, 0, (struct sockaddr *)client, addr_len);    // Envois du signal pour commencer à envoyer/recevoir
    
    while (1) {
        int retries = 0, len;
        while (retries < MAX_RETRIES) {
            len = recvfrom(sock, buffer, PACKET_SIZE, 0, (struct sockaddr *)client, &addr_len);     // Reception du packet
            if (len < 4) {  // Si problème
                printf("[ATTENTION] Timeout ! Réessai... (%d/%d)\n", retries + 1, MAX_RETRIES);
                retries++;
                continue;
            }
            break;
        }
        if (retries == MAX_RETRIES) {
            printf("[ERREUR] Abandon après %d tentatives.\n", MAX_RETRIES);
            close(file);
            return;
        }
        
        if (ntohs(*(short *)buffer) != OP_DATA || ntohs(*(short *)(buffer + 2)) != block + 1) break;
        
        write(file, buffer + 4, len - 4);
        block++;    // Bloc suivant
        
        *(short *)buffer = htons(OP_ACK);
        *(short *)(buffer + 2) = htons(block);
        sendto(sock, buffer, 4, 0, (struct sockaddr *)client, addr_len);    // Informe que le client peut envoyer le packet suivant
        printf("[INFO] Reçu et confirmé bloc %d\n", block);
        
        if (len < PACKET_SIZE) break;   // Si plus rien dans le fichier, on arrête
    }
    printf("[INFO] Réception terminée.\n");
    close(file);
}

void send_error(int sock, struct sockaddr_in *client, int code, char *msg) {
    char buffer[PACKET_SIZE];
    *(short *)buffer = htons(OP_ERROR);
    *(short *)(buffer + 2) = htons(code);
    strcpy(buffer + 4, msg);
    sendto(sock, buffer, 4 + strlen(msg) + 1, 0, (struct sockaddr *)client, sizeof(*client));
    printf("[ERREUR] %s\n", msg);
}
