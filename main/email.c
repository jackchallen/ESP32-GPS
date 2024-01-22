/*
 * SMTP email client
 *
 * Adapted from the `ssl_mail_client` example in mbedtls.
 *
 * SPDX-FileCopyrightText: The Mbed TLS Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPDX-FileContributor: 2015-2021 Espressif Systems (Shanghai) CO LTD
 */
#include <revk.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
//#include "protocol_examples_common.h"

#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/esp_debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include <mbedtls/base64.h>
#include <sys/param.h>

#ifdef  CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#else
#include "lecert.h"
#endif

extern char *emailhost;
extern char *emailuser;
extern char *emailpass;
extern char *emailport;
extern char *emailfrom;

#define SERVER_USES_STARTSSL 1

static const char *TAG = "Email";

#define BUF_SIZE            512

#define VALIDATE_MBEDTLS_RETURN(ret, min_valid_ret, max_valid_ret, goto_label)  \
    do {                                                                        \
        if (ret < min_valid_ret || ret > max_valid_ret) {                       \
            goto goto_label;                                                    \
        }                                                                       \
    } while (0)                                                                 \

/**
 * Root cert for smtp.googlemail.com, taken from server_root_cert.pem
 *
 * The PEM file was extracted from the output of this command:
 * openssl s_client -showcerts -connect smtp.googlemail.com:587 -starttls smtp
 *
 * The CA root cert is the last cert given in the chain of certs.
 *
 * To embed it in the app binary, the PEM file is named
 * in the component.mk COMPONENT_EMBED_TXTFILES variable.
 */

static int
write_and_get_response (mbedtls_net_context * sock_fd, unsigned char *buf, size_t len)
{
   int ret;
   const size_t DATA_SIZE = 128;
   unsigned char data[DATA_SIZE];
   char code[4];
   size_t i,
     idx = 0;

   if (len)
   {
      ESP_LOGD (TAG, "%s", buf);
   }

   if (len && (ret = mbedtls_net_send (sock_fd, buf, len)) <= 0)
   {
      ESP_LOGE (TAG, "mbedtls_net_send failed with error -0x%x", -ret);
      return ret;
   }

   do
   {
      len = DATA_SIZE - 1;
      memset (data, 0, DATA_SIZE);
      ret = mbedtls_net_recv (sock_fd, data, len);

      if (ret <= 0)
      {
         ESP_LOGE (TAG, "mbedtls_net_recv failed with error -0x%x", -ret);
         goto exit;
      }

      data[len] = '\0';
      printf ("\n%s", data);
      len = ret;
      for (i = 0; i < len; i++)
      {
         if (data[i] != '\n')
         {
            if (idx < 4)
            {
               code[idx++] = data[i];
            }
            continue;
         }

         if (idx == 4 && code[0] >= '0' && code[0] <= '9' && code[3] == ' ')
         {
            code[3] = '\0';
            ret = atoi (code);
            goto exit;
         }

         idx = 0;
      }
   }
   while (1);

 exit:
   return ret;
}

static int
write_ssl_and_get_response (mbedtls_ssl_context * ssl, unsigned char *buf, size_t len)
{
   int ret;
   const size_t DATA_SIZE = 128;
   unsigned char data[DATA_SIZE];
   char code[4];
   size_t i,
     idx = 0;

   if (len)
   {
      ESP_LOGD (TAG, "%s", buf);
   }

   while (len && (ret = mbedtls_ssl_write (ssl, buf, len)) <= 0)
   {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
      {
         ESP_LOGE (TAG, "mbedtls_ssl_write failed with error -0x%x", -ret);
         goto exit;
      }
   }

   do
   {
      len = DATA_SIZE - 1;
      memset (data, 0, DATA_SIZE);
      ret = mbedtls_ssl_read (ssl, data, len);

      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
      {
         continue;
      }

      if (ret <= 0)
      {
         ESP_LOGE (TAG, "mbedtls_ssl_read failed with error -0x%x", -ret);
         goto exit;
      }

      ESP_LOGD (TAG, "%s", data);

      len = ret;
      for (i = 0; i < len; i++)
      {
         if (data[i] != '\n')
         {
            if (idx < 4)
            {
               code[idx++] = data[i];
            }
            continue;
         }

         if (idx == 4 && code[0] >= '0' && code[0] <= '9' && code[3] == ' ')
         {
            code[3] = '\0';
            ret = atoi (code);
            goto exit;
         }

         idx = 0;
      }
   }
   while (1);

 exit:
   return ret;
}

static int
write_ssl_data (mbedtls_ssl_context * ssl, unsigned char *buf, size_t len)
{
   int ret;

   if (len)
   {
      ESP_LOGD (TAG, "%s", buf);
   }

   while (len && (ret = mbedtls_ssl_write (ssl, buf, len)) <= 0)
   {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
      {
         ESP_LOGE (TAG, "mbedtls_ssl_write failed with error -0x%x", -ret);
         return ret;
      }
   }

   return 0;
}

static int
perform_tls_handshake (mbedtls_ssl_context * ssl)
{
   int ret = -1;
   uint32_t flags;
   char *buf = mallocspi (BUF_SIZE);
   if (!buf)
      return 599;

   ESP_LOGI (TAG, "Performing the SSL/TLS handshake...");

   fflush (stdout);
   while ((ret = mbedtls_ssl_handshake (ssl)) != 0)
   {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
      {
         ESP_LOGE (TAG, "mbedtls_ssl_handshake returned -0x%x", -ret);
         goto exit;
      }
   }

   ESP_LOGI (TAG, "Verifying peer X.509 certificate...");

   if ((flags = mbedtls_ssl_get_verify_result (ssl)) != 0)
   {
      /* In real life, we probably want to close connection if ret != 0 */
      ESP_LOGW (TAG, "Failed to verify peer certificate!");
      mbedtls_x509_crt_verify_info (buf, BUF_SIZE, "  ! ", flags);
      ESP_LOGW (TAG, "verification info: %s", buf);
   } else
   {
      ESP_LOGI (TAG, "Certificate verified.");
   }

   ESP_LOGI (TAG, "Cipher suite is %s", mbedtls_ssl_get_ciphersuite (ssl));
   ret = 0;                     /* No error */

 exit:
   free (buf);
   return ret;
}

int
email_send (const char *emailto, const char *contenttype, const char *subject, FILE * i)
{
   if (!*emailhost)
      return 599;
   char *buf = NULL;
   unsigned char base64_buffer[128];
   int ret,
     len;
   size_t base64_len;

   mbedtls_entropy_context entropy;
   mbedtls_ctr_drbg_context ctr_drbg;
   mbedtls_ssl_context ssl;
   mbedtls_x509_crt cacert;
   mbedtls_ssl_config conf;
   mbedtls_net_context server_fd;

   mbedtls_ssl_init (&ssl);
   //mbedtls_x509_crt_init(&cacert);
   mbedtls_ctr_drbg_init (&ctr_drbg);
   ESP_LOGI (TAG, "Seeding the random number generator");

   mbedtls_ssl_config_init (&conf);
   esp_crt_bundle_attach (&conf);

   mbedtls_entropy_init (&entropy);
   if ((ret = mbedtls_ctr_drbg_seed (&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0)) != 0)
   {
      ESP_LOGE (TAG, "mbedtls_ctr_drbg_seed returned -0x%x", -ret);
      goto exit;
   }

   ESP_LOGI (TAG, "Setting hostname for TLS session...");

   /* Hostname set here should match CN in server certificate */
   if ((ret = mbedtls_ssl_set_hostname (&ssl, emailhost)) != 0)
   {
      ESP_LOGE (TAG, "mbedtls_ssl_set_hostname returned -0x%x", -ret);
      goto exit;
   }

   ESP_LOGI (TAG, "Setting up the SSL/TLS structure...");

   if ((ret = mbedtls_ssl_config_defaults (&conf,
                                           MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
   {
      ESP_LOGE (TAG, "mbedtls_ssl_config_defaults returned -0x%x", -ret);
      goto exit;
   }

   mbedtls_ssl_conf_authmode (&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
   mbedtls_ssl_conf_ca_chain (&conf, &cacert, NULL);
   mbedtls_ssl_conf_rng (&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
#ifdef CONFIG_MBEDTLS_DEBUG
   mbedtls_esp_enable_debug_log (&conf, 4);
#endif

   if ((ret = mbedtls_ssl_setup (&ssl, &conf)) != 0)
   {
      ESP_LOGE (TAG, "mbedtls_ssl_setup returned -0x%x", -ret);
      goto exit;
   }

   mbedtls_net_init (&server_fd);

   ESP_LOGI (TAG, "Connecting to %s:%s...", emailhost, emailport);

   if ((ret = mbedtls_net_connect (&server_fd, emailhost, emailport, MBEDTLS_NET_PROTO_TCP)) != 0)
   {
      ESP_LOGE (TAG, "mbedtls_net_connect returned -0x%x", -ret);
      goto exit;
   }

   ESP_LOGI (TAG, "Connected.");

   mbedtls_ssl_set_bio (&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

   buf = mallocspi (BUF_SIZE);
   if (!buf)
      goto exit;
#if SERVER_USES_STARTSSL
   /* Get response */
   ret = write_and_get_response (&server_fd, (unsigned char *) buf, 0);
   VALIDATE_MBEDTLS_RETURN (ret, 200, 299, exit);

   ESP_LOGI (TAG, "Writing EHLO to server...");
   len = snprintf ((char *) buf, BUF_SIZE, "EHLO %s\r\n", "ESP32");
   ret = write_and_get_response (&server_fd, (unsigned char *) buf, len);
   VALIDATE_MBEDTLS_RETURN (ret, 200, 299, exit);

   ESP_LOGI (TAG, "Writing STARTTLS to server...");
   len = snprintf ((char *) buf, BUF_SIZE, "STARTTLS\r\n");
   ret = write_and_get_response (&server_fd, (unsigned char *) buf, len);
   VALIDATE_MBEDTLS_RETURN (ret, 200, 299, exit);

   ret = perform_tls_handshake (&ssl);
   if (ret != 0)
   {
      goto exit;
   }
#else /* SERVER_USES_STARTSSL */
   ret = perform_tls_handshake (&ssl);
   if (ret != 0)
   {
      goto exit;
   }

   /* Get response */
   ret = write_ssl_and_get_response (&ssl, (unsigned char *) buf, 0);
   VALIDATE_MBEDTLS_RETURN (ret, 200, 299, exit);
   ESP_LOGI (TAG, "Writing EHLO to server...");

   len = snprintf ((char *) buf, BUF_SIZE, "EHLO %s\r\n", "ESP32");
   ret = write_ssl_and_get_response (&ssl, (unsigned char *) buf, len);
   VALIDATE_MBEDTLS_RETURN (ret, 200, 299, exit);

#endif /* SERVER_USES_STARTSSL */

   if (*emailuser)
   {
      /* Authentication */
      ESP_LOGI (TAG, "Authentication...");

      ESP_LOGI (TAG, "Write AUTH LOGIN");
      len = snprintf ((char *) buf, BUF_SIZE, "AUTH LOGIN\r\n");
      ret = write_ssl_and_get_response (&ssl, (unsigned char *) buf, len);
      VALIDATE_MBEDTLS_RETURN (ret, 200, 399, exit);

      ESP_LOGI (TAG, "Write USER NAME");
      ret = mbedtls_base64_encode ((unsigned char *) base64_buffer, sizeof (base64_buffer),
                                   &base64_len, (unsigned char *) emailuser, strlen (emailuser));
      if (ret != 0)
      {
         ESP_LOGE (TAG, "Error in mbedtls encode! ret = -0x%x", -ret);
         goto exit;
      }
      len = snprintf ((char *) buf, BUF_SIZE, "%s\r\n", base64_buffer);
      ret = write_ssl_and_get_response (&ssl, (unsigned char *) buf, len);
      VALIDATE_MBEDTLS_RETURN (ret, 300, 399, exit);

      ESP_LOGI (TAG, "Write PASSWORD");
      ret = mbedtls_base64_encode ((unsigned char *) base64_buffer, sizeof (base64_buffer),
                                   &base64_len, (unsigned char *) emailpass, strlen (emailpass));
      if (ret != 0)
      {
         ESP_LOGE (TAG, "Error in mbedtls encode! ret = -0x%x", -ret);
         goto exit;
      }
      len = snprintf ((char *) buf, BUF_SIZE, "%s\r\n", base64_buffer);
      ret = write_ssl_and_get_response (&ssl, (unsigned char *) buf, len);
      VALIDATE_MBEDTLS_RETURN (ret, 200, 399, exit);
   }

   /* Compose email */
   ESP_LOGI (TAG, "Write MAIL FROM");
   len = snprintf ((char *) buf, BUF_SIZE, "MAIL FROM:<%s>\r\n", emailfrom);
   ret = write_ssl_and_get_response (&ssl, (unsigned char *) buf, len);
   VALIDATE_MBEDTLS_RETURN (ret, 200, 299, exit);

   ESP_LOGI (TAG, "Write RCPT");
   len = snprintf ((char *) buf, BUF_SIZE, "RCPT TO:<%s>\r\n", emailto);
   ret = write_ssl_and_get_response (&ssl, (unsigned char *) buf, len);
   VALIDATE_MBEDTLS_RETURN (ret, 200, 299, exit);

   ESP_LOGI (TAG, "Write DATA");
   len = snprintf ((char *) buf, BUF_SIZE, "DATA\r\n");
   ret = write_ssl_and_get_response (&ssl, (unsigned char *) buf, len);
   VALIDATE_MBEDTLS_RETURN (ret, 300, 399, exit);

   ESP_LOGI (TAG, "Write Content");
   /* We do not take action if message sending is partly failed. */
   len = snprintf ((char *) buf, BUF_SIZE, "From: %s <%s>\r\n"  //
                   "Subject: %s\r\n"    //
                   "To: <%s>\r\n"       //
                   "MIME-Version: 1.0 (mime-construct 1.9)\n",  //
                   revk_id, emailfrom, subject, emailto);

    /**
     * Note: We are not validating return for some ssl_writes.
     * If by chance, it's failed; at worst email will be incomplete!
     */
   ret = write_ssl_data (&ssl, (unsigned char *) buf, len);

   /* Multipart boundary */
   len = snprintf ((char *) buf, BUF_SIZE, "Content-Type: multipart/mixed;boundary=XYZabcd1234\n" "--XYZabcd1234\n");
   ret = write_ssl_data (&ssl, (unsigned char *) buf, len);

   /* Text */
   len = snprintf ((char *) buf, BUF_SIZE,
                   "Content-Type: text/plain\n"
                   "This is a simple test mail from the SMTP client example.\r\n" "\r\n" "Enjoy!\n\n--XYZabcd1234\n");
   ret = write_ssl_data (&ssl, (unsigned char *) buf, len);

   // Send data

   len = snprintf ((char *) buf, BUF_SIZE, "\n--XYZabcd1234\n");
   ret = write_ssl_data (&ssl, (unsigned char *) buf, len);

   len = snprintf ((char *) buf, BUF_SIZE, "\r\n.\r\n");
   ret = write_ssl_and_get_response (&ssl, (unsigned char *) buf, len);
   VALIDATE_MBEDTLS_RETURN (ret, 200, 299, exit);
   ESP_LOGI (TAG, "Email sent!");

   /* Close connection */
   mbedtls_ssl_close_notify (&ssl);
   ret = 0;                     /* No errors */

 exit:
   ESP_LOGE (TAG, "Ret=%d", ret);
   mbedtls_net_free (&server_fd);
   mbedtls_ssl_free (&ssl);
   mbedtls_ssl_config_free (&conf);
   mbedtls_ctr_drbg_free (&ctr_drbg);
   mbedtls_entropy_free (&entropy);

   if (ret != 0)
   {
      mbedtls_strerror (ret, buf, 100);
      ESP_LOGE (TAG, "Last error was: -0x%x - %s", -ret, buf);
   }

   putchar ('\n');              /* Just a new line */
   if (buf)
   {
      free (buf);
   }
   return ret;
}
