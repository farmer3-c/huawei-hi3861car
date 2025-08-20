#include "hi_wifi_api.h"
#include "lwip/ip_addr.h"
#include "lwip/netifapi.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ohos_init.h"
#include <errno.h>
#include "cmsis_os2.h"
#include "cJSON.h"

#include "car_test.h" // Assuming this header defines get_car_status, set_car_status, set_car_mode, and CAR_STATUS/MODE enums

// Client address storage (for sending responses)
// WARNING: This variable is shared between udp_thread and status_send_thread.
// Without a mutex, there is a risk of race conditions and data corruption
// if both threads access it concurrently.
static struct sockaddr_in client_addr;
static socklen_t client_addr_len = sizeof(client_addr);
static int send_sockfd = -1; // Sending socket
// Mutex removed as per user request.
// static osMutexId_t client_addr_mutex = NULL; // Mutex to protect client_addr
static int consecutive_failures = 0; // Consecutive send failure counter
const int MAX_FAILURES = 10;         // Max consecutive failures before socket reset
char recvline[1024];

/**
 * @brief Sends the car's current status via UDP.
 *
 * This function constructs a JSON string with the car's status and sends it
 * to the last known client address.
 *
 * @param status A string representing the car's current status (e.g., "forward", "stop").
 * @return 0 on success, -1 on failure.
 */
int udp_send_car_status(const char *status, const char *speed)
{
    printf("Enter udp_send_car_status, status: %s\n", status);

    // Check if the sending socket is initialized and client address is known
    if (send_sockfd < 0 || client_addr.sin_addr.s_addr == INADDR_ANY ||
        client_addr.sin_port == 0) // Also check if port is set
    {
        printf("UDP send not initialized or client address unknown/invalid\n");
        return -1;
    }

    // Construct JSON format status data
    char send_buf[128] = {0};
    // Ensure the buffer is large enough for the JSON string
    snprintf(send_buf, sizeof(send_buf), "{\"status\":\"%s\", \"speed\":\"%s\"}", status, speed);

    // WARNING: client_addr is accessed here without protection.
    int ret = sendto(send_sockfd, send_buf, strlen(send_buf), 0,
                     (struct sockaddr *)&client_addr, client_addr_len);

    if (ret < 0)
    {
        // Print detailed error information if send fails
        printf("Failed to send status: %d, errno=%d, %s\n",
               ret, errno, strerror(errno));
    }
    else
    {
        printf("Status sent successfully: %s to %s:%d\n", send_buf,
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }

    return ret; // Return the result of sendto
}

/**
 * @brief Thread for periodically sending car status updates.
 *
 * This thread initializes a UDP sending socket, binds it to a fixed port,
 * and then periodically sends the car's current status. It also includes logic
 * to reset the socket if too many consecutive send failures occur.
 *
 * @param arg Unused argument.
 */
void status_send_thread(void *arg)
{
    (void)arg; // Cast to void to suppress unused parameter warning

    // Create sending socket
    send_sockfd = socket(PF_INET, SOCK_DGRAM, 0);
    if (send_sockfd < 0)
    {
        printf("Failed to create sending socket\n");
        return; // Exit thread if socket creation fails
    }

    struct sockaddr_in send_addr = {0};
    send_addr.sin_family = AF_INET;
    send_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    send_addr.sin_port = htons(50002); // Fixed sending port 50002 (car sends FROM this port)

    // Removed: client_addr.sin_port = htons(50002);
    // This line was incorrectly trying to set the *destination* port for status updates
    // in the sending thread. The destination port should be set by the receiving thread
    // based on the client's known listening port.

    // Bind the sending socket
    if (bind(send_sockfd, (struct sockaddr *)&send_addr, sizeof(send_addr)) < 0)
    {
        printf("Failed to bind sending port\n");
        close(send_sockfd); // Close socket on bind failure
        send_sockfd = -1;   // Invalidate socket descriptor
        return;             // Exit thread
    }

    printf("Status sending thread started\n");

    while (1)
    {
        osDelay(500); // Send status every 500ms

        // Check if socket needs to be reset due to too many failures
        if (consecutive_failures >= MAX_FAILURES)
        {
            printf("Too many consecutive failures, resetting socket\n");
            if (send_sockfd >= 0)
            {
                close(send_sockfd);
            }

            // Attempt to recreate the socket
            send_sockfd = socket(PF_INET, SOCK_DGRAM, 0);
            if (send_sockfd < 0)
            {
                printf("Failed to recreate socket\n");
                consecutive_failures++; // Continue incrementing to prevent infinite loop if recreation consistently fails
                continue;               // Skip sending this cycle
            }

            // Rebind the new socket
            if (bind(send_sockfd, (struct sockaddr *)&send_addr, sizeof(send_addr)) < 0)
            {
                printf("Failed to rebind sending port after reset\n");
                close(send_sockfd);
                send_sockfd = -1;
                consecutive_failures++;
                continue;
            }

            consecutive_failures = 0; // Reset failure counter on successful socket reset
        }

        // Get current car status from car_test.h/c
        char *status = get_car_status();
        char *speed = get_car_speed();
        if (status != NULL && speed != NULL)
        {
            if (udp_send_car_status(status, speed) < 0)
            {
                consecutive_failures++; // Increment on send failure
            }
            else
            {
                consecutive_failures = 0; // Reset on successful send
            }
        }
    }
}

/**
 * @brief Main UDP receiving thread for car control commands.
 *
 * This thread initializes a UDP receiving socket, binds it to port 50001,
 * and continuously listens for incoming commands. Upon receiving a command,
 * it parses the JSON data, updates the car's status or mode, and saves the
 * client's address for sending responses.
 *
 * @param pdata Unused argument.
 */
void udp_thread(void *pdata)
{
    int ret;
    struct sockaddr_in servaddr;
    cJSON *recvjson;

    (void)pdata; // Cast to void to suppress unused parameter warning

    // Create receiving socket
    int sockfd = socket(PF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        printf("Failed to create receiving socket\n");
        return; // Exit thread on socket creation failure
    }

    bzero(&servaddr, sizeof(servaddr)); // Clear server address structure
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all available interfaces
    servaddr.sin_port = htons(50001);             // Listening port 50001 (car listens ON this port)

    printf("UDP thread started\n");

    // Bind the receiving socket
    ret = bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    if (ret < 0)
    {
        printf("Failed to bind socket\n");
        close(sockfd); // Close socket on bind failure
        return;        // Exit thread
    }

    while (1)
    {
        struct sockaddr_in addrClient;
        socklen_t sizeClientAddr = sizeof(struct sockaddr_in); // Use socklen_t for size

        // Initialize receive buffer before each recvfrom call
        memset(recvline, 0, sizeof(recvline));

        // Receive data from any client
        ret = recvfrom(sockfd, recvline, sizeof(recvline) - 1, 0,
                       (struct sockaddr *)&addrClient, &sizeClientAddr); // Pass pointer to socklen_t

        if (ret > 0)
        {
            recvline[ret] = '\0'; // Null-terminate the received string
            char *pClientIP = inet_ntoa(addrClient.sin_addr);

            // Print client information and received data
            printf("Client %s:%d says: %s\n", pClientIP, ntohs(addrClient.sin_port), recvline);

            // WARNING: client_addr is written here without protection.
            // This can lead to race conditions if status_send_thread is reading it concurrently.
            // Only copy the IP address and family from the incoming packet.
            client_addr.sin_addr = addrClient.sin_addr;
            client_addr.sin_family = addrClient.sin_family;
            client_addr_len = sizeClientAddr; // Keep the length, though it's usually constant for IPv4

            // *** CRITICAL FIX: Explicitly set the destination port for status updates
            // to the known C# client listening port (50002).
            // This ensures status is sent to the correct port on the client. ***
            client_addr.sin_port = htons(50002);

            // Print the saved client address for verification
            char client_ip_str[INET_ADDRSTRLEN]; // Buffer for IP address string
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip_str, sizeof(client_ip_str));
            printf("Saved client address for status updates: %s:%d\n",
                   client_ip_str, ntohs(client_addr.sin_port));

            // Parse received JSON command
            recvjson = cJSON_Parse(recvline);
            if (recvjson != NULL)
            {
                cJSON *cmd = cJSON_GetObjectItem(recvjson, "cmd");
                if (cmd != NULL && cJSON_IsString(cmd) && cmd->valuestring != NULL)
                {
                    printf("Command received: %s\n", cmd->valuestring);

                    // Process car control commands
                    if (strcmp("forward", cmd->valuestring) == 0)
                    {
                        set_car_status(CAR_STATUS_FORWARD);
                        printf("Moving forward\n");
                    }
                    else if (strcmp("backward", cmd->valuestring) == 0)
                    {
                        set_car_status(CAR_STATUS_BACKWARD);
                        printf("Moving backward\n");
                    }
                    else if (strcmp("left", cmd->valuestring) == 0)
                    {
                        set_car_status(CAR_STATUS_LEFT);
                        printf("Turning left\n");
                    }
                    else if (strcmp("right", cmd->valuestring) == 0)
                    {
                        set_car_status(CAR_STATUS_RIGHT);
                        printf("Turning right\n");
                    }
                    else if (strcmp("stop", cmd->valuestring) == 0)
                    {
                        set_car_status(CAR_STATUS_STOP);
                        printf("Stopped\n");
                    }
                    else
                    {
                        printf("Unknown command: %s\n", cmd->valuestring);
                    }
                }

                cJSON *mode = cJSON_GetObjectItem(recvjson, "mode");
                if (mode != NULL && cJSON_IsString(mode) && mode->valuestring != NULL)
                {
                    // Process car mode commands
                    if (strcmp("step", mode->valuestring) == 0)
                    {
                        set_car_mode(CAR_MODE_STEP);
                        printf("Mode set to STEP\n");
                    }
                    else if (strcmp("alway", mode->valuestring) == 0) // Note: "alway" might be a typo, should it be "always"?
                    {
                        set_car_mode(CAR_MODE_ALWAY);
                        printf("Mode set to ALWAY\n");
                    }
                    else
                    {
                        printf("Unknown mode: %s\n", mode->valuestring);
                    }
                }

                cJSON *speed = cJSON_GetObjectItem(recvjson, "speed");
                if (speed != NULL && cJSON_IsString(speed) && speed->valuestring != NULL)
                {
                    printf("Speed received: %s\n", speed->valuestring);

                    // 根据收到的speed值设置对应的车速
                    if (strcmp("low", speed->valuestring) == 0)
                    {
                        set_car_speed(CAR_SPEED_LOW);
                        printf("Speed set to low\n");
                    }
                    else if (strcmp("medium", speed->valuestring) == 0)
                    {
                        set_car_speed(CAR_SPEED_MEDIUM);
                        printf("Speed set to medium\n");
                    }
                    else if (strcmp("high", speed->valuestring) == 0)
                    {
                        set_car_speed(CAR_SPEED_HIGH);
                        printf("Speed set to high\n");
                    }
                    else
                    {
                        // 未知车速时默认设为中速
                        set_car_speed(CAR_SPEED_MEDIUM);
                        printf("Unknown speed: %s, default to medium\n", speed->valuestring);
                    }
                }
                else
                {
                    // 如果没有收到speed字段，默认使用中速
                    set_car_speed(CAR_SPEED_MEDIUM);
                    printf("No speed received, default to medium\n");
                }

                cJSON_Delete(recvjson); // Free cJSON object
            }
            else
            {
                printf("Failed to parse JSON: %s\n", recvline);
            }
        }
        else if (ret < 0)
        {
            printf("recvfrom failed: errno=%d, %s\n", errno, strerror(errno));
            // Depending on the error, you might want to close and recreate the socket,
            // or add a delay to prevent a tight loop on persistent errors.
            osDelay(100); // Add a small delay to prevent busy-waiting on errors
        }
        // If ret == 0, it means the connection was gracefully closed, which is unlikely for UDP.
    }
}

/**
 * @brief Starts the UDP receiving and status sending threads.
 *
 * This function creates and launches two threads: one for receiving UDP commands
 * and another for periodically sending car status updates.
 */
void start_udp_thread(void)
{
    osThreadAttr_t attr_recv = {0}; // Initialize to zero
    attr_recv.name = "udp_recv_thread";
    attr_recv.stack_size = 10240; // Sufficient stack size for UDP operations and cJSON parsing
    attr_recv.priority = 36;      // High priority for command reception

    if (osThreadNew((osThreadFunc_t)udp_thread, NULL, &attr_recv) == NULL)
    {
        printf("[CarControl] Failed to create UDP receiving thread!\n");
    }

    osThreadAttr_t attr_send = {0}; // Initialize to zero
    attr_send.name = "status_send_thread";
    attr_send.stack_size = 10240;               // Sufficient stack size
    attr_send.priority = osPriorityBelowNormal; // Lower priority for periodic status updates

    if (osThreadNew((osThreadFunc_t)status_send_thread, NULL, &attr_send) == NULL)
    {
        printf("[CarControl] Failed to create status sending thread!\n");
    }
}
