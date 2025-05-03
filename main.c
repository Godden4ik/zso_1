#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <string.h>

// Compilation flags
// Uncomment to enable debug prints
#define DEBUG_PRINT

// Uncomment to enable debug delays
// #define DEBUG_SLEEP

// Error checking macro for pthread functions
#define CHECK_PTHREAD_RETURN(x, msg) \
    do { \
        int ret = (x); \
        if (ret != 0) { \
            fprintf(stderr, "%s failed: %s\n", msg, strerror(ret)); \
            exit(EXIT_FAILURE); \
        } \
    } while(0)

// Constants (parameterizable at compilation level)
#define NUM_CLASSES 5
#define STUDENTS_PER_CLASS 20
#define TOTAL_STUDENTS (NUM_CLASSES * STUDENTS_PER_CLASS)
#define MIN_STUDENTS_FOR_LESSON 10
#define NUM_TEACHERS NUM_CLASSES
#define REQUIRED_LESSONS 3
#define LESSON_DURATION 3  // in seconds, only used when DEBUG_SLEEP is defined

// Logging levels
#define LOG_INFO    0
#define LOG_DEBUG   1
#define LOG_VERBOSE 2
#define CURRENT_LOG_LEVEL LOG_INFO

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

// Helper function to print debug messages with log levels
void log_message(int level, const char* format, ...) {
#ifdef DEBUG_PRINT
    if (level <= CURRENT_LOG_LEVEL) {
        va_list args;
        va_start(args, format);

        // Add timestamp - use thread-safe version
        time_t now = time(NULL);
        struct tm tm_now_buf;  // Create a local buffer for the time structure
        struct tm *tm_now = localtime_r(&now, &tm_now_buf);  // Thread-safe version
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "[%H:%M:%S] ", tm_now);

        // Lock for printf to ensure atomic console output
        static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&log_mutex);

        printf("%s", timestamp);

        // Print the actual message
        vprintf(format, args);

        pthread_mutex_unlock(&log_mutex);
        va_end(args);
    }
#endif
}

// Helper function to introduce delays
void debug_sleep(int seconds) {
#ifdef DEBUG_SLEEP
    sleep(seconds);
#endif
}

// Helper function to check if a student has already attended a classroom
bool student_already_attended_classroom(int student_id, int classroom_id, int lessons_attended) {
    for (int i = 0; i < lessons_attended; i++) {
        if (student_lesson_history[student_id][i] == classroom_id) {
            return true;
        }
    }
    return false;
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

        CHECK_PTHREAD_RETURN(pthread_mutex_init(&classrooms[i].mutex, NULL),
                            "Classroom mutex initialization");
        CHECK_PTHREAD_RETURN(pthread_cond_init(&classrooms[i].lesson_start_cv, NULL),
                            "Classroom start condition initialization");
        CHECK_PTHREAD_RETURN(pthread_cond_init(&classrooms[i].lesson_end_cv, NULL),
                            "Classroom end condition initialization");
    }

    CHECK_PTHREAD_RETURN(pthread_mutex_init(&school_mutex, NULL),
                        "School mutex initialization");
}

// Clean up resources
void cleanup_resources() {
    for (int i = 0; i < NUM_CLASSES; i++) {
        CHECK_PTHREAD_RETURN(pthread_mutex_destroy(&classrooms[i].mutex),
                            "Classroom mutex destruction");
        CHECK_PTHREAD_RETURN(pthread_cond_destroy(&classrooms[i].lesson_start_cv),
                            "Classroom start condition destruction");
        CHECK_PTHREAD_RETURN(pthread_cond_destroy(&classrooms[i].lesson_end_cv),
                            "Classroom end condition destruction");
    }

    CHECK_PTHREAD_RETURN(pthread_mutex_destroy(&school_mutex),
                        "School mutex destruction");
}

// Check if there are enough students left in school for a regular lesson
// Caller MUST hold school_mutex before calling this function
bool enough_students_for_regular_lesson() {
    return students_in_school >= MIN_STUDENTS_FOR_LESSON;
}

// Helper to get students in school safely
int get_students_in_school() {
    int count;
    CHECK_PTHREAD_RETURN(pthread_mutex_lock(&school_mutex),
                        "get_students_in_school: lock");
    count = students_in_school;
    CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&school_mutex),
                        "get_students_in_school: unlock");
    return count;
}

// Teacher thread function
void* teacher_function(void* arg) {
    int teacher_id = *((int*)arg);
    free(arg);

    log_message(LOG_INFO, "Teacher %d has arrived at school.\n", teacher_id);

    int classroom_id = teacher_id; // Each teacher has a designated classroom
    int lessons_taught = 0;

    while (lessons_taught < REQUIRED_LESSONS) {
        log_message(LOG_INFO, "Teacher %d preparing for lesson %d in classroom %d.\n",
                   teacher_id, lessons_taught + 1, classroom_id);

        CHECK_PTHREAD_RETURN(pthread_mutex_lock(&classrooms[classroom_id].mutex),
                            "Teacher: classroom mutex lock");

        // Mark this classroom as having a teacher
        classrooms[classroom_id].teacher_id = teacher_id;
        classrooms[classroom_id].state = LESSON_WAITING;

        // Wait for enough students or special condition
        bool start_with_fewer = false;

        while (classrooms[classroom_id].students_count < MIN_STUDENTS_FOR_LESSON) {
            // Acquire school_mutex to check students_in_school safely
            CHECK_PTHREAD_RETURN(pthread_mutex_lock(&school_mutex),
                                "Teacher: school mutex lock");

            // Special condition: start with fewer students if not enough in school
            if (!enough_students_for_regular_lesson()) {
                start_with_fewer = true;
                CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&school_mutex),
                                    "Teacher: school mutex unlock (special cond)");
                break;
            }

            CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&school_mutex),
                                "Teacher: school mutex unlock");

            // Wait for more students
            log_message(LOG_DEBUG, "Teacher %d waiting for students. Current count: %d\n",
                      teacher_id, classrooms[classroom_id].students_count);

            CHECK_PTHREAD_RETURN(pthread_cond_wait(&classrooms[classroom_id].lesson_start_cv,
                                &classrooms[classroom_id].mutex),
                                "Teacher: waiting for students");
        }

        // Start the lesson
        classrooms[classroom_id].state = LESSON_IN_PROGRESS;
        log_message(LOG_INFO, "Teacher %d starting lesson in classroom %d with %d students%s.\n",
                   teacher_id, classroom_id, classrooms[classroom_id].students_count,
                   start_with_fewer ? " (fewer than required)" : "");

        // Signal all students that the lesson has started
        CHECK_PTHREAD_RETURN(pthread_cond_broadcast(&classrooms[classroom_id].lesson_start_cv),
                            "Teacher: broadcasting lesson start");

        CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&classrooms[classroom_id].mutex),
                            "Teacher: classroom mutex unlock after starting");

        // Conduct the lesson
        debug_sleep(LESSON_DURATION);

        // End the lesson
        CHECK_PTHREAD_RETURN(pthread_mutex_lock(&classrooms[classroom_id].mutex),
                            "Teacher: classroom mutex lock for ending");

        classrooms[classroom_id].state = LESSON_ENDED;
        log_message(LOG_INFO, "Teacher %d ending lesson in classroom %d.\n",
                   teacher_id, classroom_id);

        // Record this lesson
        teacher_lesson_history[teacher_id][lessons_taught] = classroom_id;
        lessons_taught++;
        teacher_lessons_taught[teacher_id] = lessons_taught;

        // Signal all students that the lesson has ended
        CHECK_PTHREAD_RETURN(pthread_cond_broadcast(&classrooms[classroom_id].lesson_end_cv),
                            "Teacher: broadcasting lesson end");

        CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&classrooms[classroom_id].mutex),
                            "Teacher: classroom mutex unlock after ending");

        // Short break between lessons
        debug_sleep(1);
        sched_yield();  // Give other threads a chance to run

        // Reset the classroom for the next lesson
        CHECK_PTHREAD_RETURN(pthread_mutex_lock(&classrooms[classroom_id].mutex),
                            "Teacher: classroom mutex lock for reset");

        classrooms[classroom_id].students_count = 0;
        for (int i = 0; i < TOTAL_STUDENTS; i++) {
            classrooms[classroom_id].students_inside[i] = 0;
        }
        classrooms[classroom_id].teacher_id = -1;

        CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&classrooms[classroom_id].mutex),
                            "Teacher: classroom mutex unlock after reset");
    }

    // Teacher has taught required number of lessons
    CHECK_PTHREAD_RETURN(pthread_mutex_lock(&school_mutex),
                        "Teacher: school mutex lock for exit");

    remaining_teachers--;
    log_message(LOG_INFO, "Teacher %d has completed all required lessons and is leaving. Teachers remaining: %d\n",
               teacher_id, remaining_teachers);

    CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&school_mutex),
                        "Teacher: school mutex unlock after exit");

    return NULL;
}

// Student thread function
void* student_function(void* arg) {
    int student_id = *((int*)arg);
    free(arg);

    log_message(LOG_INFO, "Student %d has arrived at school.\n", student_id);

    int lessons_attended = 0;

    while (lessons_attended < REQUIRED_LESSONS) {
        // Check if any teachers are left in the school
        CHECK_PTHREAD_RETURN(pthread_mutex_lock(&school_mutex),
                            "Student: school mutex lock");

        if (remaining_teachers == 0) {
            // No teachers left, student should leave
            students_in_school--;
            log_message(LOG_INFO, "Student %d is leaving because no teachers remain. Lessons attended: %d/%d\n",
                      student_id, lessons_attended, REQUIRED_LESSONS);

            CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&school_mutex),
                                "Student: school mutex unlock (no teachers)");
            return NULL;
        }

        // We're done with school_mutex for now
        CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&school_mutex),
                            "Student: school mutex unlock");

        bool found_classroom = false;
        int chosen_classroom = -1;

        // Look for an available classroom in sequential order
        // Starting from student_id % NUM_CLASSES to distribute students more evenly
        for (int offset = 0; offset < NUM_CLASSES; offset++) {
            int i = (student_id + offset) % NUM_CLASSES;

            CHECK_PTHREAD_RETURN(pthread_mutex_lock(&classrooms[i].mutex),
                                "Student: classroom mutex lock");

            if (classrooms[i].state == LESSON_WAITING &&
                classrooms[i].teacher_id != -1 &&
                !classrooms[i].students_inside[student_id]) {

                // Check if we've already attended this classroom in the past
                bool already_attended = student_already_attended_classroom(student_id, i, lessons_attended);

                if (!already_attended) {
                    // Join this classroom
                    classrooms[i].students_count++;
                    classrooms[i].students_inside[student_id] = 1;
                    chosen_classroom = i;
                    found_classroom = true;

                    log_message(LOG_INFO, "Student %d joined classroom %d. Student count: %d\n",
                               student_id, i, classrooms[i].students_count);

                    // Signal teacher if enough students have arrived
                    if (classrooms[i].students_count >= MIN_STUDENTS_FOR_LESSON) {
                        CHECK_PTHREAD_RETURN(pthread_cond_broadcast(&classrooms[i].lesson_start_cv),
                                            "Student: signaling lesson start");
                    }

                    CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&classrooms[i].mutex),
                                        "Student: classroom mutex unlock (joined)");
                    break;
                }
            }

            CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&classrooms[i].mutex),
                                "Student: classroom mutex unlock (not joined)");
        }

        if (!found_classroom) {
            // Wait briefly before trying again
            log_message(LOG_DEBUG, "Student %d couldn't find an available classroom. Waiting...\n", student_id);
            debug_sleep(1);
            sched_yield();  // Give other threads a chance to run
            continue;
        }

        // Wait for the lesson to start and end
        CHECK_PTHREAD_RETURN(pthread_mutex_lock(&classrooms[chosen_classroom].mutex),
                            "Student: classroom mutex lock (waiting for lesson)");

        // Wait if the lesson hasn't started yet
        while (classrooms[chosen_classroom].state == LESSON_WAITING) {
            // Get students_in_school safely for logging
            int total_students = get_students_in_school();

            log_message(LOG_VERBOSE, "Student %d waiting for lesson to start in classroom %d. "
                       "Amount of students in the class waiting: %d "
                       "Total amount of students in the school: %d \n",
                       student_id, chosen_classroom,
                       classrooms[chosen_classroom].students_count,
                       total_students);

            CHECK_PTHREAD_RETURN(pthread_cond_wait(&classrooms[chosen_classroom].lesson_start_cv,
                                &classrooms[chosen_classroom].mutex),
                                "Student: waiting for lesson to start");
        }

        // Participate in the lesson
        log_message(LOG_DEBUG, "Student %d participating in lesson in classroom %d.\n",
                   student_id, chosen_classroom);

        // Wait for the lesson to end
        while (classrooms[chosen_classroom].state == LESSON_IN_PROGRESS) {
            CHECK_PTHREAD_RETURN(pthread_cond_wait(&classrooms[chosen_classroom].lesson_end_cv,
                                &classrooms[chosen_classroom].mutex),
                                "Student: waiting for lesson to end");
        }

        // Record this lesson
        student_lesson_history[student_id][lessons_attended] = chosen_classroom;
        lessons_attended++;
        student_lessons_attended[student_id] = lessons_attended;

        log_message(LOG_INFO, "Student %d completed lesson in classroom %d. Lessons attended: %d/%d\n",
                   student_id, chosen_classroom, lessons_attended, REQUIRED_LESSONS);

        CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&classrooms[chosen_classroom].mutex),
                            "Student: classroom mutex unlock (lesson complete)");
    }

    // Student has attended required number of lessons
    CHECK_PTHREAD_RETURN(pthread_mutex_lock(&school_mutex),
                        "Student: school mutex lock (exit)");

    students_in_school--;
    log_message(LOG_INFO, "Student %d has completed all required lessons and is leaving. Students remaining: %d\n",
               student_id, students_in_school);

    CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&school_mutex),
                        "Student: school mutex unlock (exit)");

    // Signal all teachers that a student has left (might trigger special condition)
    for (int i = 0; i < NUM_CLASSES; i++) {
        CHECK_PTHREAD_RETURN(pthread_mutex_lock(&classrooms[i].mutex),
                            "Student: classroom mutex lock (signal on exit)");

        CHECK_PTHREAD_RETURN(pthread_cond_signal(&classrooms[i].lesson_start_cv),
                            "Student: signaling on exit");

        CHECK_PTHREAD_RETURN(pthread_mutex_unlock(&classrooms[i].mutex),
                            "Student: classroom mutex unlock (signal on exit)");
    }

    return NULL;
}

// Generate simulation statistics
void generate_simulation_stats() {
    // Count completed lessons
    int students_completed = 0;
    int teachers_completed = 0;

    for (int i = 0; i < TOTAL_STUDENTS; i++) {
        if (student_lessons_attended[i] == REQUIRED_LESSONS) {
            students_completed++;
        }
    }

    for (int i = 0; i < NUM_TEACHERS; i++) {
        if (teacher_lessons_taught[i] == REQUIRED_LESSONS) {
            teachers_completed++;
        }
    }

    // Print detailed summary
    printf("\n===== Simulation Summary =====\n");
    printf("Students who completed all lessons: %d/%d (%.1f%%)\n",
           students_completed, TOTAL_STUDENTS,
           (float)students_completed/TOTAL_STUDENTS * 100);
    printf("Teachers who completed all lessons: %d/%d (%.1f%%)\n",
           teachers_completed, NUM_TEACHERS,
           (float)teachers_completed/NUM_TEACHERS * 100);

    // Print details about lessons per student
    printf("\nLesson attendance distribution:\n");
    int attendance_count[REQUIRED_LESSONS + 1] = {0}; // Count for 0, 1, 2, 3 lessons
    for (int i = 0; i < TOTAL_STUDENTS; i++) {
        attendance_count[student_lessons_attended[i]]++;
    }

    for (int i = 0; i <= REQUIRED_LESSONS; i++) {
        printf("  Students who attended %d lessons: %d\n", i, attendance_count[i]);
    }

    // Print classroom utilization
    printf("\nClassroom utilization:\n");
    int classroom_attendance[NUM_CLASSES] = {0};

    for (int i = 0; i < TOTAL_STUDENTS; i++) {
        for (int j = 0; j < student_lessons_attended[i]; j++) {
            if (student_lesson_history[i][j] >= 0) {
                classroom_attendance[student_lesson_history[i][j]]++;
            }
        }
    }

    for (int i = 0; i < NUM_CLASSES; i++) {
        printf("  Classroom %d: %d students attended\n", i, classroom_attendance[i]);
    }
}

int main() {
    // Initialize resources
    initialize_classrooms();

    // Create teacher threads
    pthread_t teacher_threads[NUM_TEACHERS];
    for (int i = 0; i < NUM_TEACHERS; i++) {
        int* id = malloc(sizeof(int));
        if (id == NULL) {
            fprintf(stderr, "Failed to allocate memory for teacher ID\n");
            exit(EXIT_FAILURE);
        }

        *id = i;
        CHECK_PTHREAD_RETURN(pthread_create(&teacher_threads[i], NULL, teacher_function, id),
                            "Teacher thread creation");
    }

    // Create student threads
    pthread_t student_threads[TOTAL_STUDENTS];
    for (int i = 0; i < TOTAL_STUDENTS; i++) {
        int* id = malloc(sizeof(int));
        if (id == NULL) {
            fprintf(stderr, "Failed to allocate memory for student ID\n");
            exit(EXIT_FAILURE);
        }

        *id = i;
        CHECK_PTHREAD_RETURN(pthread_create(&student_threads[i], NULL, student_function, id),
                            "Student thread creation");
    }

    // Wait for all threads to finish
    for (int i = 0; i < NUM_TEACHERS; i++) {
        CHECK_PTHREAD_RETURN(pthread_join(teacher_threads[i], NULL),
                            "Teacher thread join");
    }

    for (int i = 0; i < TOTAL_STUDENTS; i++) {
        CHECK_PTHREAD_RETURN(pthread_join(student_threads[i], NULL),
                            "Student thread join");
    }

    // Generate and print statistics
    generate_simulation_stats();

    // Clean up
    cleanup_resources();

    return 0;
}