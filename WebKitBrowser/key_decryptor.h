/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2021 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __KEY_DECRYPTOR_H
#define __KEY_DECRYPTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Decrypts a private key.
 *
 * This function decrypts memory buffer with a private key and returns
 * a memory buffer containing the decrypted key in the PEM format.
 * The returned memory buffer needs to be freed with the key_decryptor_free()
 * function.
 *
 * \param encrypted_key_data - A pointer to a location where the encrypted key data is located.
 * \param encrypted_key_size - The size of the encrypted key data.
 *
 * \return NULL on decryption failure, pointer to a NULL terminated string on success.
 */
const char* key_decryptor_decrypt_pem(const uint8_t* encrypted_key_data, uint32_t encrypted_key_size);

/**
 * \brief Frees memory allocated by other functions from this API.
 *
 * \param data - Pointer to the memory to be freed.
 */
void key_decryptor_free(const char* data);

#ifdef __cplusplus
}
#endif

#endif // __KEY_DECRYPTOR_H
