/***************************************************************************
  file : $URL$
  version : $LastChangedRevision$  $LastChangedBy$
  date : $LastChangedDate$
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 * Copyright (C) 2007 by Johan De Taeye                                    *
 *                                                                         *
 * This library is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU Lesser General Public License as published   *
 * by the Free Software Foundation; either version 2.1 of the License, or  *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This library is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser *
 * General Public License for more details.                                *
 *                                                                         *
 * You should have received a copy of the GNU Lesser General Public        *
 * License along with this library; if not, write to the Free Software     *
 * Foundation Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA *
 *                                                                         *
 ***************************************************************************/

/* The code in this file is copied from the gsoap manual, with only relatively
 * small changes.
 */

#include "module.h"
#include <pthread.h>

// Include the namespace map
#include "frepple.nsmap"


namespace module_webservice
{


void CommandWebservice::execute()
{
  // Log
  if (getVerbose())
  {
    logger << "Start running the web service with " << threads
      << " threads on port " << port << " at " << Date::now() << endl;
  }
  Timer t;

  // Initialize
  thread_data threadinfo[threads];
  struct soap soap;
  soap_init(&soap);
  SOAP_SOCKET mastersocket, slavesocket;

  // Bind to a port on the local machine.
  mastersocket = soap_bind(&soap, NULL, port, BACKLOG);
  if (!soap_valid_socket(mastersocket))
    throw frepple::RuntimeException("Can't bind to port " + port);

  // Creating execution threads in the pool
  pthread_mutex_init(&queue_cs, NULL);
  pthread_cond_init(&queue_cv, NULL);
  for (int i = 0; i < threads; i++)
  {
    threadinfo[i].master = this;
    threadinfo[i].index = i;
    threadinfo[i].soap_thr = soap_copy(&soap);
    pthread_create(&threadinfo[i].tid, NULL, (void*(*)(void*))process_queue, static_cast<void*>(&threadinfo[i]));
  }

  // Message
  if (getVerbose())
    logger << "Read to serve incoming requests" << endl;

  // Loop forever
  for (;;)
  {
    // Wait for incoming connection on the port
    slavesocket = soap_accept(&soap);
    if (!soap_valid_socket(slavesocket))
    {
      if (soap.errnum)
      {
        soap_print_fault(&soap, stderr);
        continue; // retry
      }
      else
      {
        logger << "Server timed out" << endl;
        break;
      }
    }
    logger << "Connection " << ((soap.ip >> 24)&0xFF) << "."
      << ((soap.ip >> 16)&0xFF) << "." << ((soap.ip >> 8)&0xFF) << "."
      << (soap.ip&0xFF) << endl;

    // Loop until the request could be entered in the request queue
    while (enqueue(slavesocket) == SOAP_EOM)
      sleep(1);
  }

  // Message
  if (getVerbose())
    logger << "Start shutting down the server at " << Date::now() << endl;

  // Send termination signal to all threads
  for (int i = 0; i < threads; i++)
  {
    // Put termination requests in the queue
    while (enqueue(SOAP_INVALID_SOCKET) == SOAP_EOM)
      sleep(1);
  }

  // Wait for the threads to terminate
  for (int i = 0; i < threads; i++)
  {
    pthread_join(threadinfo[i].tid, NULL);
    soap_done(threadinfo[i].soap_thr);
    free(threadinfo[i].soap_thr);
  }

  // Cleaning up
  pthread_mutex_destroy(&queue_cs);
  pthread_cond_destroy(&queue_cv);
  soap_done(&soap);

  // Log
  if (getVerbose())
    logger << "Finished web service at " << Date::now() << " : " << t << endl;
}


void* CommandWebservice::process_queue(void *soap)
{
   struct thread_data *mydata = (struct thread_data*)soap;
   // Loop forever
   for (;;)
   {
      // Pick a request from my master's queue
      mydata->soap_thr->socket = mydata->master->dequeue();

      // Break out of the loop if an invalid socket is put in the queue
      if (!soap_valid_socket(mydata->soap_thr->socket)) break;

      // Process the request
      soap_serve(mydata->soap_thr);
      soap_destroy(mydata->soap_thr);
      soap_end(mydata->soap_thr);
   }
   return NULL;
}


int CommandWebservice::enqueue(SOAP_SOCKET sock)
{
   int status = SOAP_OK;
   int next;
   pthread_mutex_lock(&queue_cs);
   next = tail + 1;
   if (next >= MAX_QUEUE)
      next = 0;
   if (next == head)
      status = SOAP_EOM;
   else
   {
      queue[tail] = sock;
      tail = next;
   }
   pthread_cond_signal(&queue_cv);
   pthread_mutex_unlock(&queue_cs);
   return status;
}


SOAP_SOCKET CommandWebservice::dequeue()
{
   SOAP_SOCKET sock;
   pthread_mutex_lock(&queue_cs);
   while (head == tail) pthread_cond_wait(&queue_cv, &queue_cs);
   sock = queue[head++];
   if (head >= MAX_QUEUE)
      head = 0;
   pthread_mutex_unlock(&queue_cs);
   return sock;
}

}