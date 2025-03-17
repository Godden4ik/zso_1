#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdbool.h>

// Compilation flags
// Uncomment to enable debug prints and delays
#define DEBUG_MODE

// Constants (parameterizable at compilation level)
#define NUM_CLASSES 5
#define STUDENTS_PER_CLASS 20
#define TOTAL_STUDENTS (NUM_CLASSES * STUDENTS_PER_CLASS)
#define MIN_STUDENTS_FOR_LESSON 10
#define NUM_TEACHERS NUM_CLASSES
#define REQUIRED_LESSONS 3
#define LESSON_DURATION 3  // in seconds, only used in DEBUG_MODE

// Classroom/Lesson states
#define LESSON_WAITING 0
#define LESSON_IN_PROGRESS 1
#define LESSON_ENDED 2

// Structure for classroom data
typedef struct {
    int id;
    int state;
    int teacher_id;
    int students_count;
    int students_inside[TOTAL_STUDENTS]; // To track which students are in the classroom
    pthread_mutex_t mutex;
    pthread_cond_t lesson_start_cv;
    pthread_cond_t lesson_end_cv;
} Classroom;

// Global variables
Classroom classrooms[NUM_CLASSES];
int students_in_school = TOTAL_STUDENTS;
int remaining_teachers = NUM_TEACHERS;
pthread_mutex_t school_mutex;

// Student and teacher tracking
int student_lessons_attended[TOTAL_STUDENTS] = {0};
int teacher_lessons_taught[NUM_TEACHERS] = {0};
int student_lesson_history[TOTAL_STUDENTS][REQUIRED_LESSONS] = {{-1}};
int teacher_lesson_history[NUM_TEACHERS][REQUIRED_LESSONS] = {{-1}};

// Helper function to print debug messages
void debug_print(const char* format, ...) {
#ifdef DEBUG_MODE
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
#endif
}

// Helper function to introduce delays
void debug_sleep(int seconds) {
#ifdef DEBUG_MODE
    sleep(seconds);
#endif
}

// Initialize the classrooms
void initialize_classrooms() {
    for (int i = 0; i < NUM_CLASSES; i++) {
        classrooms[i].id = i;
        classrooms[i].state = LESSON_WAITING;
        classrooms[i].teacher_id = -1;
        classrooms[i].students_count = 0;

        for (int j = 0; j < TOTAL_STUDENTS; j++) {
            classrooms[i].students_inside[j] = 0;
        }

        pthread_mutex_init(&classrooms[i].mutex, NULL);
        pthread_cond_init(&classrooms[i].lesson_start_cv, NULL);
        pthread_cond_init(&classrooms[i].lesson_end_cv, NULL);
    }

    pthread_mutex_init(&school_mutex, NULL);
}

// Clean up resources
void cleanup_resources() {
    for (int i = 0; i < NUM_CLASSES; i++) {
        pthread_mutex_destroy(&classrooms[i].mutex);
        pthread_cond_destroy(&classrooms[i].lesson_start_cv);
        pthread_cond_destroy(&classrooms[i].lesson_end_cv);
    }

    pthread_mutex_destroy(&school_mutex);
}

// Check if there are enough students left in school for a regular lesson
bool enough_students_for_regular_lesson() {
    return students_in_school >= MIN_STUDENTS_FOR_LESSON;
}

// Teacher thread function
void* teacher_function(void* arg) {
    int teacher_id = *((int*)arg);
    free(arg);

    debug_print("Teacher %d has arrived at school.\n", teacher_id);

    int classroom_id = teacher_id; // Each teacher has a designated classroom
    int lessons_taught = 0;

    while (lessons_taught < REQUIRED_LESSONS) {
        debug_print("Teacher %d preparing for lesson %d in classroom %d.\n",
                   teacher_id, lessons_taught + 1, classroom_id);

        pthread_mutex_lock(&classrooms[classroom_id].mutex);

        // Mark this classroom as having a teacher
        classrooms[classroom_id].teacher_id = teacher_id;
        classrooms[classroom_id].state = LESSON_WAITING;

        // Wait for enough students or special condition
        bool start_with_fewer = false;

        while (classrooms[classroom_id].students_count < MIN_STUDENTS_FOR_LESSON) {
            pthread_mutex_lock(&school_mutex);
            // Special condition: start with fewer students if not enough in school
            if (!enough_students_for_regular_lesson() &&
                classrooms[classroom_id].students_count > 0) {
                start_with_fewer = true;
                pthread_mutex_unlock(&school_mutex);
                break;
            }
            pthread_mutex_unlock(&school_mutex);

            // Wait for more students
            debug_print("Teacher %d waiting for students. Current count: %d\n",
                       teacher_id, classrooms[classroom_id].students_count);
            pthread_cond_wait(&classrooms[classroom_id].lesson_start_cv,
                             &classrooms[classroom_id].mutex);
        }

        // Start the lesson
        classrooms[classroom_id].state = LESSON_IN_PROGRESS;
        debug_print("Teacher %d starting lesson in classroom %d with %d students%s.\n",
                   teacher_id, classroom_id, classrooms[classroom_id].students_count,
                   start_with_fewer ? " (fewer than required)" : "");

        pthread_mutex_unlock(&classrooms[classroom_id].mutex);

        // Conduct the lesson
        debug_sleep(LESSON_DURATION);

        // End the lesson
        pthread_mutex_lock(&classrooms[classroom_id].mutex);
        classrooms[classroom_id].state = LESSON_ENDED;
        debug_print("Teacher %d ending lesson in classroom %d.\n", teacher_id, classroom_id);

        // Record this lesson
        teacher_lesson_history[teacher_id][lessons_taught] = classroom_id;
        lessons_taught++;
        teacher_lessons_taught[teacher_id] = lessons_taught;

        // Signal all students that the lesson has ended
        pthread_cond_broadcast(&classrooms[classroom_id].lesson_end_cv);
        pthread_mutex_unlock(&classrooms[classroom_id].mutex);

        // Short break between lessons
        debug_sleep(1);

        // Reset the classroom for the next lesson
        pthread_mutex_lock(&classrooms[classroom_id].mutex);
        classrooms[classroom_id].students_count = 0;
        for (int i = 0; i < TOTAL_STUDENTS; i++) {
            classrooms[classroom_id].students_inside[i] = 0;
        }
        classrooms[classroom_id].teacher_id = -1;
        pthread_mutex_unlock(&classrooms[classroom_id].mutex);
    }

    // Teacher has taught required number of lessons
    pthread_mutex_lock(&school_mutex);
    remaining_teachers--;
    debug_print("Teacher %d has completed all required lessons and is leaving. Teachers remaining: %d\n",
               teacher_id, remaining_teachers);
    pthread_mutex_unlock(&school_mutex);

    return NULL;
}

// Student thread function
void* student_function(void* arg) {
    int student_id = *((int*)arg);
    free(arg);

    debug_print("Student %d has arrived at school.\n", student_id);

    int lessons_attended = 0;

    while (lessons_attended < REQUIRED_LESSONS) {
        bool found_classroom = false;
        int chosen_classroom = -1;

        // Look for an available classroom
        for (int i = 0; i < NUM_CLASSES; i++) {
            pthread_mutex_lock(&classrooms[i].mutex);

            if (classrooms[i].state == LESSON_WAITING &&
                classrooms[i].teacher_id != -1 &&
                !classrooms[i].students_inside[student_id]) {

                // Check if we've already attended this classroom in the past
                bool already_attended = false;
                for (int j = 0; j < lessons_attended; j++) {
                    if (student_lesson_history[student_id][j] == i) {
                        already_attended = true;
                        break;
                    }
                }

                if (!already_attended) {
                    // Join this classroom
                    classrooms[i].students_count++;
                    classrooms[i].students_inside[student_id] = 1;
                    chosen_classroom = i;
                    found_classroom = true;

                    debug_print("Student %d joined classroom %d. Student count: %d\n",
                               student_id, i, classrooms[i].students_count);

                    // Signal teacher if enough students have arrived
                    if (classrooms[i].students_count >= MIN_STUDENTS_FOR_LESSON) {
                        pthread_cond_signal(&classrooms[i].lesson_start_cv);
                    }

                    pthread_mutex_unlock(&classrooms[i].mutex);
                    break;
                }
            }

            pthread_mutex_unlock(&classrooms[i].mutex);
        }

        if (!found_classroom) {
            // Wait briefly before trying again
            debug_print("Student %d couldn't find an available classroom. Waiting...\n", student_id);
            debug_sleep(1);
            continue;
        }

        // Wait for the lesson to start and end
        pthread_mutex_lock(&classrooms[chosen_classroom].mutex);

        // Wait if the lesson hasn't started yet
        while (classrooms[chosen_classroom].state == LESSON_WAITING) {
            debug_print("Student %d waiting for lesson to start in classroom %d.\n",
                       student_id, chosen_classroom);
            pthread_cond_wait(&classrooms[chosen_classroom].lesson_start_cv,
                             &classrooms[chosen_classroom].mutex);
        }

        // Participate in the lesson
        debug_print("Student %d participating in lesson in classroom %d.\n",
                   student_id, chosen_classroom);

        // Wait for the lesson to end
        while (classrooms[chosen_classroom].state == LESSON_IN_PROGRESS) {
            pthread_cond_wait(&classrooms[chosen_classroom].lesson_end_cv,
                             &classrooms[chosen_classroom].mutex);
        }

        // Record this lesson
        student_lesson_history[student_id][lessons_attended] = chosen_classroom;
        lessons_attended++;
        student_lessons_attended[student_id] = lessons_attended;

        debug_print("Student %d completed lesson in classroom %d. Lessons attended: %d/%d\n",
                   student_id, chosen_classroom, lessons_attended, REQUIRED_LESSONS);

        pthread_mutex_unlock(&classrooms[chosen_classroom].mutex);
    }

    // Student has attended required number of lessons
    pthread_mutex_lock(&school_mutex);
    students_in_school--;
    debug_print("Student %d has completed all required lessons and is leaving. Students remaining: %d\n",
               student_id, students_in_school);

    // Signal all teachers that a student has left (might trigger special condition)
    for (int i = 0; i < NUM_CLASSES; i++) {
        pthread_cond_signal(&classrooms[i].lesson_start_cv);
    }

    pthread_mutex_unlock(&school_mutex);

    return NULL;
}

int main() {
    // Initialize resources
    initialize_classrooms();

    // Create teacher threads
    pthread_t teacher_threads[NUM_TEACHERS];
    for (int i = 0; i < NUM_TEACHERS; i++) {
        int* id = malloc(sizeof(int));
        *id = i;
        pthread_create(&teacher_threads[i], NULL, teacher_function, id);
    }

    // Create student threads
    pthread_t student_threads[TOTAL_STUDENTS];
    for (int i = 0; i < TOTAL_STUDENTS; i++) {
        int* id = malloc(sizeof(int));
        *id = i;
        pthread_create(&student_threads[i], NULL, student_function, id);
    }

    // Wait for all threads to finish
    for (int i = 0; i < NUM_TEACHERS; i++) {
        pthread_join(teacher_threads[i], NULL);
    }

    for (int i = 0; i < TOTAL_STUDENTS; i++) {
        pthread_join(student_threads[i], NULL);
    }

    // Clean up
    cleanup_resources();

    // Print summary
    debug_print("\n===== Simulation Summary =====\n");
    debug_print("All students and teachers have completed their required lessons.\n");

    return 0;
}
