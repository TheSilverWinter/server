#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <errno.h>

#define TFTP_PORT 6969
#define BUFFER_SIZE 516  // 2 octets opcode, 2 octets numéro de bloc, 512 octets de données
#define DATA_SIZE 512

// Codes TFTP
#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5

// Code d'erreur TFTP
#define ERR_FILE_NOT_FOUND 1

// Mutex global pour protéger l'accès aux fichiers
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// Déclaration de la structure pour les arguments de thread
struct thread_args {
    int sock;  // Socket principale (pour envoyer d'éventuels paquets d'erreur)
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    unsigned char buffer[BUFFER_SIZE];
    ssize_t received_bytes;
};

// Typedef pour simplifier l'utilisation de la structure
typedef struct thread_args thread_args_t;

// Prototypes des fonctions de thread
void *handle_wrq(void *args);
void *handle_rrq(void *args);

// Gestion de la requête WRQ
void *handle_wrq(void *args) {
    thread_args_t *targs = (thread_args_t *)args;  // Conversion du paramètre
    char filename[256];
    char mode[12];
    int index = 2; // après l'opcode

    // Construction du chemin complet : "Server/<nom_fichier>"
    snprintf(filename, sizeof(filename), "Server/");
    strncat(filename, (char *)(targs->buffer + index), sizeof(filename) - strlen(filename) - 1);

    // Avancer l'index jusqu'au '\0' qui termine le nom de fichier
    while(index < targs->received_bytes && targs->buffer[index] != '\0')
        index++;
    index++; // passer le '\0'
    strncpy(mode, (char *)(targs->buffer + index), sizeof(mode) - 1);
    mode[sizeof(mode) - 1] = '\0';

    printf("[WRQ] Demande d'écriture pour le fichier '%s' en mode %s\n", filename, mode);

    // Création d'une socket dédiée pour cette session
    int sock_thread = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_thread < 0) {
        perror("[WRQ] socket (thread)");
        free(targs);
        pthread_exit(NULL);
    }

    // Envoi de l'ACK initial (bloc 0)
    unsigned char ack[4] = {0, OP_ACK, 0, 0};
    if(sendto(sock_thread, ack, 4, 0, (struct sockaddr *)&targs->client_addr, targs->addr_len) < 0)
        perror("[WRQ] sendto ACK initial");
    else
        printf("[WRQ] Envoi de l'ACK initial (bloc 0) à %s:%d\n",
               inet_ntoa(targs->client_addr.sin_addr), ntohs(targs->client_addr.sin_port));

    // Ouverture du fichier en écriture avec protection
    pthread_mutex_lock(&file_mutex);
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("[WRQ] fopen");
        pthread_mutex_unlock(&file_mutex);
        close(sock_thread);
        free(targs);
        pthread_exit(NULL);
    }

    uint16_t expected_block = 1;
    int finished = 0;
    while (!finished) {
        unsigned char data_packet[BUFFER_SIZE];
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        ssize_t n = recvfrom(sock_thread, data_packet, BUFFER_SIZE, 0,
                             (struct sockaddr *)&client, &client_len);
        if (n < 0) {
            perror("[WRQ] recvfrom");
            break;
        }
        uint16_t opcode = (((unsigned char)data_packet[0]) << 8) | ((unsigned char)data_packet[1]);
        if (opcode != OP_DATA) {
            printf("[WRQ] Paquet reçu non DATA (opcode %d)\n", opcode);
            continue;
        }
        uint16_t block = (((unsigned char)data_packet[2]) << 8) | ((unsigned char)data_packet[3]);
        printf("[WRQ] Reçu bloc %d (attendu %d), taille données = %ld octets\n", block, expected_block, n - 4);
        if (block != expected_block) {
            printf("[WRQ] Bloc inattendu : %d au lieu de %d\n", block, expected_block);
            continue;
        }
        size_t data_len = n - 4;
        fwrite(data_packet + 4, 1, data_len, fp);
        fflush(fp);

        ack[2] = data_packet[2];
        ack[3] = data_packet[3];
        if(sendto(sock_thread, ack, 4, 0, (struct sockaddr *)&client, client_len) < 0)
            perror("[WRQ] sendto ACK");
        else
            printf("[WRQ] Envoi de l'ACK pour le bloc %d\n", block);
        expected_block++;
        if (data_len < DATA_SIZE)
            finished = 1;
    }
    fclose(fp);
    pthread_mutex_unlock(&file_mutex);
    close(sock_thread);
    printf("[WRQ] Transfert terminé pour '%s'\n", filename);
    free(targs);
    pthread_exit(NULL);
}

// Gestion de la requête RRQ (lecture)
void *handle_rrq(void *args) {
    thread_args_t *targs = (thread_args_t *)args;  // Conversion du paramètre
    char filename[256];
    char mode[12];
    int index = 2; // après l'opcode

    snprintf(filename, sizeof(filename), "Server/");
    strncat(filename, (char *)(targs->buffer + index), sizeof(filename) - strlen(filename) - 1);

    while(index < targs->received_bytes && targs->buffer[index] != '\0')
        index++;
    index++; // passer le '\0'
    strncpy(mode, (char *)(targs->buffer + index), sizeof(mode) - 1);
    mode[sizeof(mode) - 1] = '\0';

    printf("[RRQ] Demande de lecture pour le fichier '%s' en mode %s\n", filename, mode);

    pthread_mutex_lock(&file_mutex);
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        pthread_mutex_unlock(&file_mutex);
        unsigned char err_pkt[BUFFER_SIZE];
        int err_index = 0;
        err_pkt[err_index++] = 0;
        err_pkt[err_index++] = OP_ERROR;
        err_pkt[err_index++] = 0;
        err_pkt[err_index++] = ERR_FILE_NOT_FOUND;
        const char *err_msg = "File not found";
        strcpy((char *)&err_pkt[err_index], err_msg);
        err_index += strlen(err_msg) + 1;
        if(sendto(targs->sock, err_pkt, err_index, 0,
                  (struct sockaddr *)&targs->client_addr, targs->addr_len) < 0)
            perror("[RRQ] sendto erreur");
        printf("[RRQ] Fichier '%s' non trouvé, envoi de l'erreur\n", filename);
        free(targs);
        pthread_exit(NULL);
    }
    pthread_mutex_unlock(&file_mutex);

    int sock_thread = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_thread < 0) {
        perror("[RRQ] socket (thread)");
        fclose(fp);
        free(targs);
        pthread_exit(NULL);
    }

    uint16_t block = 1;
    int finished = 0;
    unsigned char data_packet[BUFFER_SIZE];
    while (!finished) {
        size_t nread = fread(data_packet + 4, 1, DATA_SIZE, fp);
        data_packet[0] = 0;
        data_packet[1] = OP_DATA;
        data_packet[2] = block >> 8;
        data_packet[3] = block & 0xFF;
        ssize_t packet_size = nread + 4;
        if(sendto(sock_thread, data_packet, packet_size, 0,
                  (struct sockaddr *)&targs->client_addr, targs->addr_len) < 0) {
            perror("[RRQ] sendto DATA");
            break;
        }
        printf("[RRQ] Envoi du bloc %d, taille = %ld octets\n", block, nread);

        unsigned char ack[4];
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        ssize_t ack_bytes = recvfrom(sock_thread, ack, 4, 0,
                                     (struct sockaddr *)&client, &client_len);
        if (ack_bytes < 0) {
            perror("[RRQ] recvfrom ACK");
            break;
        }
        uint16_t ack_opcode = (((unsigned char)ack[0]) << 8) | ((unsigned char)ack[1]);
        uint16_t ack_block  = (((unsigned char)ack[2]) << 8) | ((unsigned char)ack[3]);
        printf("[RRQ] Reçu ACK pour le bloc %d\n", ack_block);
        if (ack_opcode != OP_ACK || ack_block != block) {
            printf("[RRQ] ACK inattendu : opcode %d, bloc %d\n", ack_opcode, ack_block);
            continue;
        }
        block++;
        if (nread < DATA_SIZE)
            finished = 1;
    }
    fclose(fp);
    close(sock_thread);
    printf("[RRQ] Transfert terminé pour '%s'\n", filename);
    free(targs);
    pthread_exit(NULL);
}

int main(void) {
    struct stat st = {0};
    if (stat("Server", &st) == -1) {
        if(mkdir("Server", 0777) < 0) {
            perror("mkdir Server");
            exit(EXIT_FAILURE);
        }
        printf("Dossier 'Server' créé.\n");
    } else {
        printf("Dossier 'Server' existant.\n");
    }
    
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TFTP_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    printf("Serveur TFTP démarré sur le port %d\n", TFTP_PORT);

    while (1) {
        thread_args_t *args = malloc(sizeof(thread_args_t));
        if (!args) {
            perror("malloc");
            continue;
        }
        args->addr_len = client_len;
        args->received_bytes = recvfrom(sockfd, args->buffer, BUFFER_SIZE, 0,
                                        (struct sockaddr *)&client_addr, &client_len);
        if (args->received_bytes < 0) {
            perror("recvfrom");
            free(args);
            continue;
        }
        args->client_addr = client_addr;
        args->sock = sockfd;
        
        uint16_t opcode = (((unsigned char)args->buffer[0]) << 8) | ((unsigned char)args->buffer[1]);
        pthread_t thread_id;
        if (opcode == OP_WRQ) {
            printf("Requête WRQ reçue de %s:%d\n",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            if (pthread_create(&thread_id, NULL, handle_wrq, args) != 0) {
                perror("pthread_create WRQ");
                free(args);
            }
        } else if (opcode == OP_RRQ) {
            printf("Requête RRQ reçue de %s:%d\n",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            if (pthread_create(&thread_id, NULL, handle_rrq, args) != 0) {
                perror("pthread_create RRQ");
                free(args);
            }
        } else {
            printf("Requête TFTP inconnue (opcode %d) reçue de %s:%d\n",
                   opcode, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            free(args);
        }
        pthread_detach(thread_id);
    }
    
    close(sockfd);
    return 0;
}

