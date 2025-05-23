#include "osi/include/list.h"

#include <bluetooth/log.h>

#include "osi/include/allocator.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

using namespace bluetooth;

struct list_node_t {
  struct list_node_t* next;
  void* data;
};

typedef struct list_t {
  list_node_t* head;
  list_node_t* tail;
  size_t length;
  list_free_cb free_cb;
  const allocator_t* allocator;
} list_t;

static list_node_t* list_free_node_(list_t* list, list_node_t* node);

// Hidden constructor, only to be used by the hash map for the allocation
// tracker.
// Behaves the same as |list_new|, except you get to specify the allocator.
list_t* list_new_internal(list_free_cb callback, const allocator_t* zeroed_allocator) {
  list_t* list = (list_t*)zeroed_allocator->alloc(sizeof(list_t));
  if (!list) {
    return NULL;
  }

  list->free_cb = callback;
  list->allocator = zeroed_allocator;
  return list;
}

list_t* list_new(list_free_cb callback) { return list_new_internal(callback, &allocator_calloc); }

void list_free(list_t* list) {
  if (!list) {
    return;
  }

  list_clear(list);
  list->allocator->free(list);
}

bool list_is_empty(const list_t* list) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  return list->length == 0;
}

bool list_contains(const list_t* list, const void* data) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  log::assert_that(data != NULL, "assert failed: data != NULL");

  for (const list_node_t* node = list_begin(list); node != list_end(list); node = list_next(node)) {
    if (list_node(node) == data) {
      return true;
    }
  }

  return false;
}

size_t list_length(const list_t* list) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  return list->length;
}

void* list_front(const list_t* list) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  log::assert_that(!list_is_empty(list), "assert failed: !list_is_empty(list)");

  return list->head->data;
}

void* list_back(const list_t* list) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  log::assert_that(!list_is_empty(list), "assert failed: !list_is_empty(list)");

  return list->tail->data;
}

list_node_t* list_back_node(const list_t* list) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  log::assert_that(!list_is_empty(list), "assert failed: !list_is_empty(list)");

  return list->tail;
}

bool list_insert_after(list_t* list, list_node_t* prev_node, void* data) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  log::assert_that(prev_node != NULL, "assert failed: prev_node != NULL");
  log::assert_that(data != NULL, "assert failed: data != NULL");

  list_node_t* node = (list_node_t*)list->allocator->alloc(sizeof(list_node_t));
  if (!node) {
    return false;
  }

  node->next = prev_node->next;
  node->data = data;
  prev_node->next = node;
  if (list->tail == prev_node) {
    list->tail = node;
  }
  ++list->length;
  return true;
}

bool list_prepend(list_t* list, void* data) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  log::assert_that(data != NULL, "assert failed: data != NULL");

  list_node_t* node = (list_node_t*)list->allocator->alloc(sizeof(list_node_t));
  if (!node) {
    return false;
  }
  node->next = list->head;
  node->data = data;
  list->head = node;
  if (list->tail == NULL) {
    list->tail = list->head;
  }
  ++list->length;
  return true;
}

bool list_append(list_t* list, void* data) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  log::assert_that(data != NULL, "assert failed: data != NULL");

  list_node_t* node = (list_node_t*)list->allocator->alloc(sizeof(list_node_t));
  if (!node) {
    return false;
  }
  node->next = NULL;
  node->data = data;
  if (list->tail == NULL) {
    list->head = node;
    list->tail = node;
  } else {
    list->tail->next = node;
    list->tail = node;
  }
  ++list->length;
  return true;
}

bool list_remove(list_t* list, void* data) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  log::assert_that(data != NULL, "assert failed: data != NULL");

  if (list_is_empty(list)) {
    return false;
  }

  if (list->head->data == data) {
    list_node_t* next = list_free_node_(list, list->head);
    if (list->tail == list->head) {
      list->tail = next;
    }
    list->head = next;
    return true;
  }

  for (list_node_t *prev = list->head, *node = list->head->next; node;
       prev = node, node = node->next) {
    if (node->data == data) {
      prev->next = list_free_node_(list, node);
      if (list->tail == node) {
        list->tail = prev;
      }
      return true;
    }
  }

  return false;
}

void list_clear(list_t* list) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  for (list_node_t* node = list->head; node;) {
    node = list_free_node_(list, node);
  }
  list->head = NULL;
  list->tail = NULL;
  list->length = 0;
}

list_node_t* list_foreach(const list_t* list, list_iter_cb callback, void* context) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  log::assert_that(callback != NULL, "assert failed: callback != NULL");

  for (list_node_t* node = list->head; node;) {
    list_node_t* next = node->next;
    if (!callback(node->data, context)) {
      return node;
    }
    node = next;
  }
  return NULL;
}

list_node_t* list_begin(const list_t* list) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  return list->head;
}

list_node_t* list_end(const list_t* list) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  return NULL;
}

list_node_t* list_next(const list_node_t* node) {
  log::assert_that(node != NULL, "assert failed: node != NULL");
  return node->next;
}

void* list_node(const list_node_t* node) {
  log::assert_that(node != NULL, "assert failed: node != NULL");
  return node->data;
}

static list_node_t* list_free_node_(list_t* list, list_node_t* node) {
  log::assert_that(list != NULL, "assert failed: list != NULL");
  log::assert_that(node != NULL, "assert failed: node != NULL");

  list_node_t* next = node->next;

  if (list->free_cb) {
    list->free_cb(node->data);
  }
  list->allocator->free(node);
  --list->length;

  return next;
}
