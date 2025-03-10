#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/file.h>  // Pour flock()

#define TFTP_PORT 6969
#define PACKET_SIZE 516    // 2 octets opcode, 2 octets numéro de bloc, 512 octets de données
#define DATA_SIZE 512

// Codes TFTP
#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5

// Code d'erreur
#define ERR_FILE_NOT_FOUND 1

// Délai d'inactivité pour fermer une session (en secondes)
#define SESSION_TIMEOUT 2

// Structure pour une session de transfert
typedef struct session {
    int sock;                      // Socket dédiée pour cette session
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    int opcode;                    // OP_RRQ ou OP_WRQ
    FILE *fp;                      // Fichier ouvert pour lecture (RRQ) ou écriture (WRQ)
    int block;                     // Numéro de bloc actuel
    int finished;                  // Indique si la session est terminée
    time_t last_activity;          // "Timestamp" de la dernière activité sur cette session
    struct session *next;
} session_t;

session_t *session_list = NULL;

// Ajouter une session à la "liste"
void add_session(session_t *sess) {
    sess->next = session_list;
    session_list = sess;
}


// "Supprimer" une session
void remove_session(session_t *sess) {
    session_t **p = &session_list;
    while (*p) {
        if (*p == sess) {
            *p = sess->next;
            return;
        }
        p = &(*p)->next;
    }
}

// Envoyer code d'erreur
void send_error(int sock, struct sockaddr_in *client, socklen_t addr_len, int err_code, const char *msg) {
    unsigned char buffer[PACKET_SIZE];
    int len = 0;
    buffer[len++] = 0;
    buffer[len++] = OP_ERROR;
    buffer[len++] = 0;
    buffer[len++] = err_code;
    strcpy((char*)(buffer + len), msg);
    len += strlen(msg) + 1;
    sendto(sock, buffer, len, 0, (struct sockaddr *)client, addr_len);
}

// Fonction qui gère la réception d'une nouvelle requête sur le socket principal
void handle_new_request(int main_sock) {
    unsigned char buffer[PACKET_SIZE];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);
    int n = recvfrom(main_sock, buffer, PACKET_SIZE, 0, (struct sockaddr *)&client, &client_len);
    if (n < 4)
        return;
    
    // On récupère l'opcode (le 2ème octet, le premier étant 0)
    int opcode = buffer[1];
    printf("Nouvelle requête %s reçue de %s:%d\n",
           (opcode == OP_RRQ) ? "RRQ" : (opcode == OP_WRQ ? "WRQ" : "INCONNU"),
           inet_ntoa(client.sin_addr), ntohs(client.sin_port));
    
    // Création d'une socket dédiée pour la session (port temporaire)
    int newsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (newsock < 0)
        return;
    struct sockaddr_in temp = {0};
    temp.sin_family = AF_INET;
    temp.sin_addr.s_addr = INADDR_ANY;
    temp.sin_port = 0;
    if (bind(newsock, (struct sockaddr *)&temp, sizeof(temp)) < 0) {
        close(newsock);
        return;
    }
    
    // Allouer et initialiser une nouvelle session
    session_t *sess = malloc(sizeof(session_t));
    sess->sock = newsock;
    sess->client_addr = client;
    sess->addr_len = client_len;
    sess->opcode = opcode;
    sess->block = (opcode == OP_RRQ) ? 1 : 0;  // Pour RRQ, début à 1 ; pour WRQ, on enverra ACK 0
    sess->finished = 0;
    sess->fp = NULL;
    sess->next = NULL;
    sess->last_activity = time(NULL);
    
    // Extraction du nom de fichier et du mode (à partir de l'offset 2)
    char filename[256], mode[12];
    int idx = 2, j = 0;
    while (idx < n && buffer[idx] != 0 && j < 255) {
        filename[j++] = buffer[idx++];
    }
    filename[j] = '\0';
    idx++; // Passer le 0
    j = 0;
    while (idx < n && buffer[idx] != 0 && j < 11) {
        mode[j++] = buffer[idx++];
    }
    mode[j] = '\0';
    printf("Session: fichier '%s', mode '%s'\n", filename, mode);
    
    if (opcode == OP_RRQ) {
        // Pour RRQ : ouvrir le fichier pour lecture
        char path[300];
        snprintf(path, sizeof(path), "Server/%s", filename);
        sess->fp = fopen(path, "rb");
        if (!sess->fp) {
            printf("Fichier '%s' non trouvé.\n", path);
            send_error(main_sock, &client, client_len, ERR_FILE_NOT_FOUND, "File not found");
            close(newsock);
            free(sess);
            return;
        }
        // Envoyer immédiatement le premier bloc
        unsigned char data[PACKET_SIZE];
        size_t bytes = fread(data + 4, 1, DATA_SIZE, sess->fp);
        data[0] = 0;
        data[1] = OP_DATA;
        data[2] = (sess->block >> 8) & 0xFF;
        data[3] = sess->block & 0xFF;
        sendto(newsock, data, bytes + 4, 0, (struct sockaddr *)&client, client_len);
        printf("Session RRQ: Envoyé bloc %d (%ld octets)\n", sess->block, bytes);
        sess->block++;
        if (bytes < DATA_SIZE)
            sess->finished = 1;
    }
    else if (opcode == OP_WRQ) {
        // Pour WRQ : ouvrir le fichier pour écriture avec verrouillage
        char path[300];
        snprintf(path, sizeof(path), "Server/%s", filename);
        // Ouvrir le fichier en mode écriture (création/troncature)
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (fd < 0) {
            send_error(main_sock, &client, client_len, 2, "L'ouverture du fichier pour l'écriture a échouée");
            close(newsock);
            free(sess);
            return;
        }
        // Essayer d'obtenir un verrou exclusif non bloquant
        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
            send_error(main_sock, &client, client_len, 3, "flock a échoué");
            close(fd);
            close(newsock);
            free(sess);
            return;
        }
        // Convertir le descripteur en FILE*
        sess->fp = fdopen(fd, "wb");
        if (!sess->fp) {
            send_error(main_sock, &client, client_len, 4, "fdopen pour l'écriture a échoué");
            close(fd);
            close(newsock);
            free(sess);
            return;
        }
        unsigned char ack[4] = {0, OP_ACK, 0, 0};
        sendto(newsock, ack, 4, 0, (struct sockaddr *)&client, client_len);
    }
    
    add_session(sess);
}

// Fonction de traitement d'une session active
void process_session(session_t *sess) {
    unsigned char buffer[PACKET_SIZE];
    int n = recvfrom(sess->sock, buffer, PACKET_SIZE, 0, NULL, NULL);
    if (n < 0)
        return;
    
    // Mettre à jour le timestamp d'activité
    sess->last_activity = time(NULL);
    
    if (sess->opcode == OP_RRQ) {
        // Pour RRQ, attendre un ACK du client pour le bloc précédent, puis envoyer le bloc suivant.
        if (n < 4)
            return;
        int ack_opcode = buffer[1];
        int ack_block = (((unsigned char)buffer[2]) << 8) | ((unsigned char)buffer[3]);
        // On attend un ACK pour le bloc précédent (bloc envoyé en dernier, c'est-à-dire bloc = courant - 1)
        if (ack_opcode == OP_ACK && ack_block == sess->block - 1) {
            unsigned char data[PACKET_SIZE];
            size_t bytes = fread(data + 4, 1, DATA_SIZE, sess->fp);
            data[0] = 0;
            data[1] = OP_DATA;
            data[2] = (sess->block >> 8) & 0xFF;
            data[3] = sess->block & 0xFF;
            sendto(sess->sock, data, bytes + 4, 0, (struct sockaddr *)&sess->client_addr, sess->addr_len);
            printf("Session RRQ: Envoyé bloc %d (%ld octets)\n", sess->block, bytes);
            sess->block++;
            if (bytes < DATA_SIZE)
                sess->finished = 1;
        }
    }
    else if (sess->opcode == OP_WRQ) {
        // Pour WRQ, recevoir des paquets DATA du client.
        if (n < 4)
            return;
        int data_opcode = buffer[1];
        int block = (((unsigned char)buffer[2]) << 8) | ((unsigned char)buffer[3]);
        if (data_opcode == OP_DATA && block == sess->block + 1) {
            fwrite(buffer + 4, 1, n - 4, sess->fp);
            fflush(sess->fp);
            sess->block = block;
            unsigned char ack[4] = {0, OP_ACK, buffer[2], buffer[3]};
            sendto(sess->sock, ack, 4, 0, (struct sockaddr *)&sess->client_addr, sess->addr_len);
            printf("Session WRQ: Reçu bloc %d, ACK envoyé\n", block);
            if (n - 4 < DATA_SIZE)
                sess->finished = 1;
        }
    }
}

int main(void) {
    // Création du dossier "Server" s'il n'existe pas
    struct stat st = {0};
    if (stat("Server", &st) == -1) {
        mkdir("Server", 0777);
        printf("Dossier 'Server' créé.\n");
    } else {
        printf("Dossier 'Server' existant.\n");
    }
    
    int main_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (main_sock < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TFTP_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(main_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    
    // Mettre la socket principale en non bloquant
    int flags = fcntl(main_sock, F_GETFL, 0);
    fcntl(main_sock, F_SETFL, flags | O_NONBLOCK);
    
    fd_set read_fds;
    int maxfd = main_sock;
    printf("Serveur TFTP (select) démarré sur le port %d\n", TFTP_PORT);
    
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(main_sock, &read_fds);
        maxfd = main_sock;
        session_t *sess = session_list;
        while (sess) {
            FD_SET(sess->sock, &read_fds);
            if (sess->sock > maxfd)
                maxfd = sess->sock;
            sess = sess->next;
        }
        
        // Setup du timeout
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        
        // Appel de select pour surveiller les sockets
        int activity = select(maxfd + 1, &read_fds, NULL, NULL, &tv);
        if (activity < 0 && errno != EINTR) {
            perror("select");
            break;
        }
        
        // Si aucune activité pendant 2 secondes, vérifier le timeout pour les sessions WRQ
        if (activity == 0) {
            time_t now = time(NULL);
            session_t *cur = session_list;
            while (cur) {
                if (cur->opcode == OP_WRQ && (now - cur->last_activity) >= SESSION_TIMEOUT) {
                    printf("Timeout de la session WRQ pour %s:%d, fermeture de la session.\n",
                           inet_ntoa(cur->client_addr.sin_addr), ntohs(cur->client_addr.sin_port));
                    cur->finished = 1;
                }
                cur = cur->next;
            }
        }
        
        // Traitement des nouvelles requêtes
        if (FD_ISSET(main_sock, &read_fds)) {
            handle_new_request(main_sock);
        }
        
        // Traitement des requêtes existantes (partie 2)
        session_t *prev = NULL;
        sess = session_list;
        while (sess) {
            session_t *next = sess->next;
            
            // Traitement des requêtes encore en activité
            if (FD_ISSET(sess->sock, &read_fds)) {
                process_session(sess);
            }
            
            // Si une requête est finie, on y met fin
            if (sess->finished) {
                printf("Session terminée pour %s:%d\n", inet_ntoa(sess->client_addr.sin_addr),
                       ntohs(sess->client_addr.sin_port));
                close(sess->sock);
                if (sess->fp)
                    fclose(sess->fp);
                if (prev)
                    prev->next = next;
                else
                    session_list = next;
                free(sess);
            } else {
                prev = sess;
            }
            sess = next;
        }
    }
    
    close(main_sock);
    return 0;
}

