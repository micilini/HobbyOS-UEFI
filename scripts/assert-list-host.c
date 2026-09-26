#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ASSERT_LIST_HEADER
#define ASSERT_LIST_HEADER "../kernel/src/core/list.h"
#endif
#include ASSERT_LIST_HEADER
#ifndef ASSERT_LIST_NO_QUEUE
#include "../kernel/src/core/queue.h"
#endif

typedef struct
{
    struct list_head head_a;
    struct list_head head_b;
    struct list_head nodes[4];
} list_fixture_t;

static jmp_buf bug_jump;
static int bug_expected;
static unsigned int warning_count;
static unsigned int bug_count;
static assertion_context_t last_warning;
static assertion_context_t last_bug;
static unsigned int assertions;
static unsigned int failures;

void assertion_warn_report(const assertion_context_t *context)
{
    warning_count++;
    if (context)
        last_warning = *context;
}

void assertion_bug_report(const assertion_context_t *context)
{
    bug_count++;
    if (context)
        last_bug = *context;
    if (bug_expected)
        longjmp(bug_jump, 1);
    abort();
}

static void check(int condition, const char *message)
{
    assertions++;
    if (!condition)
    {
        failures++;
        fprintf(stderr, "[ASSERT_HOST][FAIL] %s\n", message);
    }
}

static void case_result(const char *id, unsigned int before)
{
    printf("[ASSERT_HOST][CASE] id=%s status=%s assertions=%u\n", id,
           failures == before ? "PASS" : "FAIL", assertions);
}

#if HOBBYOS_DEBUG_ASSERT
static int expect_bug(void (*operation)(void))
{
    bug_expected = 1;
    if (setjmp(bug_jump) == 0)
    {
        operation();
        bug_expected = 0;
        return 0;
    }
    bug_expected = 0;
    return 1;
}
#endif

static void fixture_init(list_fixture_t *fixture)
{
    list_init(&fixture->head_a);
    list_init(&fixture->head_b);
    for (size_t index = 0; index < 4; index++)
        list_init(&fixture->nodes[index]);
}

static list_fixture_t fixture;

#if HOBBYOS_DEBUG_ASSERT
static void operation_double_insert(void)
{
    list_add_tail(&fixture.nodes[0], &fixture.head_a);
}

static void operation_insert_other_list(void)
{
    list_add_tail(&fixture.nodes[0], &fixture.head_b);
}

static void operation_insert_broken_prev(void)
{
    __list_add(&fixture.nodes[2], &fixture.nodes[0], &fixture.nodes[1]);
}

static void operation_insert_broken_next(void)
{
    __list_add(&fixture.nodes[2], &fixture.nodes[0], &fixture.nodes[1]);
}

static void operation_insert_partial(void)
{
    list_add_tail(&fixture.nodes[0], &fixture.head_a);
}

static void operation_remove_broken_prev(void)
{
    list_del(&fixture.nodes[0]);
}

static void operation_remove_broken_next(void)
{
    list_del(&fixture.nodes[0]);
}

static void operation_double_delete(void)
{
    list_del(&fixture.nodes[0]);
}

static void operation_null_add(void)
{
    list_add(NULL, &fixture.head_a);
}

static void operation_null_head(void)
{
    list_add(&fixture.nodes[0], NULL);
}

static void operation_null_delete(void)
{
    list_del(NULL);
}

static void check_rejected_without_store(const char *label,
                                         void (*operation)(void))
{
    unsigned char before[sizeof(fixture)];
    memcpy(before, &fixture, sizeof(fixture));
    unsigned int bugs_before = bug_count;
    check(expect_bug(operation), label);
    check(bug_count == bugs_before + 1u, "fatal helper called exactly once");
    check(memcmp(before, &fixture, sizeof(fixture)) == 0,
          "rejected mutation changed fixture bytes");
}
#endif

static void test_macros(void)
{
    unsigned int before = failures;
#if HOBBYOS_DEBUG_ASSERT
    int evaluations = 0;
    unsigned int warnings_before = warning_count;
    KWARN_ON(++evaluations == 0);
    check(evaluations == 1, "false warning condition evaluated once");
    check(warning_count == warnings_before,
          "false warning condition emitted a report");

    int continuation = 0;
    unsigned int call_line = __LINE__ + 1u;
    KWARN_ON(++evaluations == 2);
    continuation = 1;
    check(evaluations == 2, "true warning condition evaluated once");
    check(continuation == 1, "warning did not continue");
    check(warning_count == warnings_before + 1u,
          "true warning report count mismatch");
    check(last_warning.line == call_line, "warning line is not the callsite");
    check(strstr(last_warning.file, "assert-list-host.c") != NULL,
          "warning file is not the callsite");
    check(strcmp(last_warning.expression, "++evaluations == 2") == 0,
          "warning expression is not the callsite expression");

    int else_path = 0;
    if (1)
        KWARN_ON(0);
    else
        else_path = 1;
    if (0)
        KBUG_ON(1);
    else
        else_path += 2;
    check(else_path == 2, "assertion macro is unsafe in if/else");

    unsigned int bugs_before = bug_count;
    bug_expected = 1;
    unsigned int bug_line = __LINE__ + 2u;
    if (setjmp(bug_jump) == 0)
        KBUG_ON(++evaluations == 3);
    bug_expected = 0;
    check(evaluations == 3, "bug condition evaluated more than once");
    check(bug_count == bugs_before + 1u, "true bug did not report");
    check(last_bug.line == bug_line, "bug line is not the callsite");
    check(strcmp(last_bug.expression, "++evaluations == 3") == 0,
          "bug expression is not the callsite expression");
#else
    int evaluations = 0;
    KWARN_ON(++evaluations);
    KBUG_ON(++evaluations);
    check(evaluations == 0, "disabled assertions evaluated an expression");
    check(warning_count == 0 && bug_count == 0,
          "disabled assertions called a helper");
#endif
    case_result("macro-contract", before);
}

static void test_valid_lists(void)
{
    unsigned int before = failures;
    fixture_init(&fixture);
    check(list_empty(&fixture.head_a), "new list is not empty");
    list_add(&fixture.nodes[0], &fixture.head_a);
    list_add_tail(&fixture.nodes[1], &fixture.head_a);
    check(fixture.head_a.next == &fixture.nodes[0], "head insertion order");
    check(fixture.head_a.prev == &fixture.nodes[1], "tail insertion order");
    check(fixture.nodes[0].next == &fixture.nodes[1], "node successor");
    list_del(&fixture.nodes[0]);
    check(fixture.nodes[0].next == NULL && fixture.nodes[0].prev == NULL,
          "delete does not create null-detached state");
    list_add_tail(&fixture.nodes[0], &fixture.head_b);
    check(fixture.head_b.next == &fixture.nodes[0],
          "null-detached node cannot be reinserted");
    list_del(&fixture.nodes[1]);
    list_add_tail(&fixture.nodes[1], &fixture.head_b);
    check(fixture.nodes[0].next == &fixture.nodes[1], "list transfer order");

    list_init(&fixture.nodes[2]);
    list_del(&fixture.nodes[2]);
    check(fixture.nodes[2].next == NULL && fixture.nodes[2].prev == NULL,
          "self-linked removal result changed");
    list_add(&fixture.nodes[2], &fixture.head_a);
    check(fixture.head_a.next == &fixture.nodes[2],
          "self-linked removal/reinsert failed");
    case_result("valid-list-states", before);
}

static void test_queue(void)
{
    unsigned int before = failures;
#ifndef ASSERT_LIST_NO_QUEUE
    queue_head_t queue;
    struct list_head nodes[3];
    queue_init(&queue);
    for (size_t index = 0; index < 3; index++)
    {
        list_init(&nodes[index]);
        queue_push(&queue, &nodes[index]);
    }
    for (size_t index = 0; index < 3; index++)
        check(queue_pop(&queue) == &nodes[index], "queue is not FIFO");
    check(queue_pop(&queue) == NULL, "empty queue pop is not null");
#endif
    case_result("queue-fifo", before);
}

#if HOBBYOS_DEBUG_ASSERT
static void test_rejections(void)
{
    unsigned int before = failures;

    fixture_init(&fixture);
    list_add_tail(&fixture.nodes[0], &fixture.head_a);
    check_rejected_without_store("adjacent double insertion accepted",
                                 operation_double_insert);

    fixture_init(&fixture);
    list_add_tail(&fixture.nodes[0], &fixture.head_a);
    list_add_tail(&fixture.nodes[1], &fixture.head_a);
    check_rejected_without_store("non-adjacent double insertion accepted",
                                 operation_double_insert);
    check_rejected_without_store("cross-list insertion accepted",
                                 operation_insert_other_list);

    fixture_init(&fixture);
    fixture.nodes[0].next = &fixture.head_a;
    fixture.nodes[0].prev = &fixture.head_a;
    check_rejected_without_store("partial detached state accepted",
                                 operation_insert_partial);

    fixture_init(&fixture);
    fixture.nodes[0].next = &fixture.head_a;
    fixture.nodes[0].prev = &fixture.head_a;
    fixture.nodes[1].prev = &fixture.nodes[0];
    fixture.nodes[1].next = &fixture.head_a;
    fixture.head_a.next = &fixture.nodes[1];
    fixture.head_a.prev = &fixture.nodes[1];
    check_rejected_without_store("broken previous reciprocity accepted",
                                 operation_insert_broken_prev);

    fixture_init(&fixture);
    fixture.nodes[0].next = &fixture.nodes[1];
    fixture.nodes[0].prev = &fixture.head_a;
    fixture.nodes[1].prev = &fixture.head_a;
    fixture.nodes[1].next = &fixture.head_a;
    fixture.head_a.next = &fixture.nodes[0];
    fixture.head_a.prev = &fixture.nodes[1];
    check_rejected_without_store("broken next reciprocity accepted",
                                 operation_insert_broken_next);

    fixture_init(&fixture);
    list_add_tail(&fixture.nodes[0], &fixture.head_a);
    fixture.head_a.next = &fixture.head_a;
    check_rejected_without_store("remove accepted broken previous neighbor",
                                 operation_remove_broken_prev);

    fixture_init(&fixture);
    list_add_tail(&fixture.nodes[0], &fixture.head_a);
    fixture.head_a.prev = &fixture.head_a;
    check_rejected_without_store("remove accepted broken next neighbor",
                                 operation_remove_broken_next);

    fixture_init(&fixture);
    list_add_tail(&fixture.nodes[0], &fixture.head_a);
    list_del(&fixture.nodes[0]);
    check_rejected_without_store("double delete accepted",
                                 operation_double_delete);

    fixture_init(&fixture);
    check_rejected_without_store("null insertion accepted",
                                 operation_null_add);
    check_rejected_without_store("null head accepted", operation_null_head);
    check(expect_bug(operation_null_delete), "null delete accepted");
    case_result("rejected-before-store", before);
}
#endif

static uint64_t sequence_state = UINT64_C(0x6173736572746c69);

static uint32_t next_sequence_value(void)
{
    sequence_state = sequence_state * UINT64_C(6364136223846793005) + 1u;
    return (uint32_t)(sequence_state >> 32);
}

static void test_deterministic_sequences(void)
{
    unsigned int before = failures;
    struct list_head head;
    struct list_head nodes[8];
    uint8_t linked[8] = {0};
    uint8_t model[8] = {0};
    size_t model_count = 0;
    list_init(&head);
    for (size_t index = 0; index < 8; index++)
        list_init(&nodes[index]);
    for (size_t step = 0; step < 2048; step++)
    {
        size_t selected = next_sequence_value() % 8u;
        if (!linked[selected])
        {
            list_add_tail(&nodes[selected], &head);
            linked[selected] = 1;
            model[model_count++] = (uint8_t)selected;
        }
        else
        {
            list_del(&nodes[selected]);
            linked[selected] = 0;
            size_t position = 0;
            while (position < model_count && model[position] != selected)
                position++;
            check(position < model_count, "model lost a linked node");
            for (; position + 1 < model_count; position++)
                model[position] = model[position + 1];
            model_count--;
        }
        size_t observed = 0;
        struct list_head *cursor;
        list_for_each(cursor, &head)
        {
            check(observed < model_count, "list has an extra node");
            if (observed < model_count)
                check(cursor == &nodes[model[observed]],
                      "list order differs from independent model");
            observed++;
        }
        check(observed == model_count, "list is missing a model node");
    }
    case_result("deterministic-sequences", before);
}

int main(void)
{
    test_macros();
    test_valid_lists();
    test_queue();
#if HOBBYOS_DEBUG_ASSERT
    test_rejections();
#endif
    test_deterministic_sequences();
    printf("[ASSERT_HOST][SUITE] status=%s assertions=%u failures=%u "
           "seed=0x6173736572746c69 sequences=2048 debug=%u\n",
           failures ? "FAIL" : "PASS", assertions, failures,
           (unsigned int)HOBBYOS_DEBUG_ASSERT);
    return failures ? 1 : 0;
}
