/**
 * @file    flatcc_port.h
 * @brief   flatcc runtime allocation hooks backed by a static arena.
 * @details The flatcc runtime is compiled with FLATCC_ALLOC/FLATCC_FREE/...
 *          mapped to these functions, which never call malloc.  The header is
 *          force-included into every translation unit so the macro expansion
 *          sees the prototypes.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     BCON-002 (flatcc runtime), BCON-003 (no malloc)
 */

#ifndef FLATCC_PORT_H
#define FLATCC_PORT_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief   Allocate a 16-byte aligned block from the static arena.
 * @param[in] size  Requested size in bytes; 0 returns NULL.
 * @return Pointer to the block, or NULL when the arena is exhausted.
 */
void *boot_flatcc_alloc(size_t size);

/**
 * @brief   Return a block to the arena and coalesce its neighbours.
 * @param[in] ptr  Block from boot_flatcc_alloc(); NULL is ignored.
 */
void boot_flatcc_free(void *ptr);

/**
 * @brief   Resize an allocation, preserving its current contents.
 * @param[in] ptr   Existing block, or NULL to allocate.
 * @param[in] size  New size; 0 frees @p ptr and returns NULL.
 * @return The resized block, or NULL on allocation failure.
 */
void *boot_flatcc_realloc(void *ptr, size_t size);

/**
 * @brief   Allocate a zero-filled array from the static arena.
 * @param[in] count  Element count.
 * @param[in] size   Element size.
 * @return Pointer to the zeroed block, or NULL on overflow/allocation
 *         failure.
 */
void *boot_flatcc_calloc(size_t count, size_t size);

/**
 * @brief   Aligned allocation; the arena always returns 16-byte alignment.
 * @param[in] alignment  Requested alignment (ignored).
 * @param[in] size       Requested size in bytes.
 * @return Pointer to the block, or NULL when the arena is exhausted.
 */
void *boot_flatcc_aligned_alloc(size_t alignment, size_t size);

/**
 * @brief   Release a block from boot_flatcc_aligned_alloc().
 * @param[in] ptr  Block to release; NULL is ignored.
 */
void boot_flatcc_aligned_free(void *ptr);

#endif /* FLATCC_PORT_H */
