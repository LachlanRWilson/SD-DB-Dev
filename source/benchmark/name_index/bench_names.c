#include "bench_names.h"

/*
 * First names, grouped by length so the distributions can pick from a range:
 * [NB_FIRST_SHORT_LO, NB_FIRST_SHORT_HI) are <= 5 letters (the key then also
 * holds part of the surname), [NB_FIRST_LONG_LO, NB_FIRST_COUNT) are >= 8
 * letters (the key is the first name alone, so every such contact with the
 * same first name shares its key).
 */
static const char *const nb_first_words[] = {
    "Aaa", "Zzz",
    "Amy", "Ben", "Cara", "Dan", "Eve", "Finn", "Gus", "Hugo", "Ian", "Jack", "Kai", "Leo", "Max", "Ned", "Oli",
    "Pip", "Ray", "Sam", "Tom", "Uma", "Val", "Will", "Zac", "Ava", "Beth", "Cody", "Dean", "Ella", "Fred", "Gina",
    "Hana", "Ivy", "Jade", "Kate", "Luke", "Mia", "Nora", "Owen", "Paul", "Rosa", "Sean", "Tara", "Vera", "Wade",
    "Yuki", "Zoe", "Adam", "Anna", "Carl", "Emma", "Ezra", "Jake", "Joel", "Lily", "Mark", "Noah", "Rhys", "Ruby",
    "Ryan", "Seth", "Alice", "Bella", "Chloe", "Daisy", "Ethan", "Grace", "Harry", "Isla", "James", "Jason", "Kevin",
    "Laura", "Liam", "Lucas", "Mason", "Nina", "Oscar", "Peter", "Quinn", "Riley", "Sarah", "Tyler", "Wendy", "Aiden",
    "Andrew", "Bianca", "Caleb", "Daniel", "Eleanor", "Fiona", "George", "Hannah", "Isaac", "Jasmine", "Joshua",
    "Lachlan", "Madison", "Natalie", "Olivia", "Patrick", "Rachel", "Sophie", "Thomas", "Vivian", "Xavier", "Yasmin",
    "Zachary", "Abigail", "Brandon", "Connor", "Declan", "Imogen", "Joseph", "Lauren", "Michael", "Nathan", "Phoebe",
    "Stephen", "Tristan",
    "Christopher", "Alexander", "Elizabeth", "Jonathan", "Samantha", "Benjamin", "Catherine", "Nicholas", "Victoria",
    "Isabella", "Margaret", "Nathaniel", "Gabriella", "Josephine", "Sebastian", "Charlotte",
};

#define NB_FIRST_COUNT ((uint16_t)(sizeof(nb_first_words) / sizeof(nb_first_words[0])))
#define NB_FIRST_LONG_LO ((uint16_t)(NB_FIRST_COUNT - 16))
#define NB_FIRST_SHORT_LO 2
#define NB_FIRST_SHORT_HI 86

static const char *const nb_last_words[] = {
    "Aaa", "Zzz", "Qzx",
    "Smith", "Jones", "Williams", "Brown", "Wilson", "Taylor", "Johnson", "White", "Martin", "Anderson", "Thompson",
    "Nguyen", "Thomas", "Walker", "Harris", "Lee", "Ryan", "Robinson", "Kelly", "King", "Davis", "Wright", "Evans",
    "Roberts", "Green", "Hall", "Wood", "Jackson", "Clarke", "Patel", "Khan", "Lewis", "James", "Phillips", "Mitchell",
    "Turner", "Campbell", "Parker", "Edwards", "Collins", "Stewart", "Morris", "Murphy", "Cook", "Rogers", "Morgan",
    "Cooper", "Peterson", "Bailey", "Reed", "Bell", "Howard", "Ward", "Cox", "Diaz", "Richardson", "Wood", "Watson",
    "Brooks", "Bennett", "Gray", "Hughes", "Price", "Sanders", "Myers", "Long", "Ross", "Foster", "Powell", "Jenkins",
    "Perry", "Russell", "Sullivan", "Butler", "Barnes", "Fisher", "Henderson", "Coleman", "Simmons", "Patterson",
    "Jordan", "Reynolds", "Hamilton", "Graham", "Kim", "Gonzales", "Alexander", "Ramos", "Wallace", "Griffin", "West",
    "Cole", "Hayes", "Chavez", "Gibson", "Bryant", "Ellis", "Stevens", "Murray", "Ford", "Marshall", "Owens", "McDonald",
    "Harrison", "Ruiz", "Kennedy", "Wells", "Alvarez", "Woods", "Mendoza", "Castillo", "Olson", "Webb", "Washington",
    "Tucker", "Freeman", "Burns", "Henry", "Vasquez", "Snyder", "Simpson", "Crawford", "Jimenez", "Porter", "Mason",
    "Shaw", "Gordon", "Wagner", "Hunter", "Romero", "Hicks", "Dixon", "Hunt", "Palmer", "Robertson", "Black", "Holmes",
    "Stone", "Meyer", "Boyd", "Mills", "Warren", "Fox", "Rose", "Rice", "Moreno", "Schmidt", "Ferguson", "Nichols",
    "Herrera", "Medina", "Ryan", "Fernandez", "Weaver", "Daniels", "Stephens", "Gardner", "Payne", "Kelley", "Dunn",
    "Pierce", "Arnold", "Tran", "Spencer", "Peters", "Hawkins", "Grant", "Hansen", "Castro", "Hoffman", "Hart",
    "Elliott", "Cunningham", "Knight", "Bradley", "Carroll", "Hudson", "Duncan", "Armstrong", "Berry", "Andrews",
    "Johnston", "Ray", "Lane", "Riley", "Carpenter", "Perkins", "Aguilar", "Silva", "Richards", "Willis", "Matthews",
    "Chapman", "Lawrence", "Garza", "Vargas", "Watkins", "Wheeler", "Larson", "Carlson", "Harper", "George", "Greene",
    "Burke", "Guzman", "Morrison", "Munoz", "Jacobs", "Obrien", "Lawson", "Franklin", "Lynch", "Bishop", "Carr",
    "Salazar", "Austin", "Mendez", "Gilbert", "Jensen", "Williamson", "Montgomery", "Harvey", "Oliver", "Howell",
    "Dean", "Hanson", "Weber", "Garrett", "Sims", "Burton", "Fuller", "Soto", "McCoy", "Welch", "Chen", "Schultz",
    "Walters", "Reid", "Fields", "Walsh", "Little", "Fowler", "Bowman", "Davidson", "May", "Day", "Schneider",
    "Newman", "Brewer", "Lucas", "Holland", "Wong", "Banks", "Santos", "Curtis", "Pearson", "Delgado", "Valdez",
    "Pena", "Rios", "Douglas", "Sandoval", "Barrett", "Hopkins", "Keller", "Guerrero", "Stanley", "Bates", "Alvarado",
    "Beck", "Ortega", "Wade", "Estrada", "Contreras", "Barnett", "Caldwell", "Santiago", "Lambert", "Powers",
    "Chambers", "Nunez", "Craig", "Leonard", "Lowe", "Rhodes", "Byrd", "Gregory", "Shelton", "Frazier", "Becker",
    "Maldonado", "Fleming", "Vega", "Sutton", "Cohen", "Jennings", "Parks", "McDaniel", "Watts", "Barker", "Norris",
    "Vaughn", "Vazquez", "Holt", "Schwartz", "Steele", "Benson", "Neal", "Dominguez", "Horton", "Terry", "Wolfe",
    "Hale", "Lyons", "Graves", "Haynes", "Miles", "Park", "Warner", "Padilla", "Bush", "Thornton", "McCarthy", "Mann",
    "Zimmerman", "Erickson", "Fletcher", "McKinney", "Page", "Dawson", "Joseph", "Marquez", "Reeves", "Klein",
    "Espinoza", "Baldwin", "Moran", "Love", "Robbins", "Higgins", "Ball", "Cortez", "Le", "Griffith", "Bowen",
    "Sharp", "Cummings", "Ramsey", "Hardy", "Swanson", "Barber", "Acosta", "Luna", "Chandler", "Blair", "Daniel",
    "Cross", "Simon", "Dennis", "Oconnor", "Quinn", "Gross", "Navarro", "Moss", "Fitzgerald", "Doyle", "McLaughlin",
    "Rojas", "Rodgers", "Stevenson", "Singh", "Yang", "Figueroa", "Harmon", "Newton", "Paul", "Manning", "Garner",
    "McGee", "Reese", "Francis", "Burgess", "Adkins", "Goodman", "Curry", "Brady", "Christensen", "Potter", "Walton",
    "Goodwin", "Mullins", "Molina", "Webster", "Fischer", "Campos", "Avila", "Sherman", "Todd", "Chang", "Blake",
    "Malone", "Wolf", "Hodges", "Juarez", "Gill", "Farmer", "Hines", "Gallagher", "Duran", "Hubbard", "Cannon",
    "Miranda", "Wang", "Saunders", "Tate", "Mack", "Hammond", "Carrillo", "Townsend", "Wise", "Ingram", "Barton",
};

#define NB_LAST_COUNT ((uint16_t)(sizeof(nb_last_words) / sizeof(nb_last_words[0])))
#define NB_LAST_LO 3

NI_PLACE NiName nb_names[NI_MAX_CONTACTS];
NI_PLACE uint8_t nb_keys[NI_MAX_CONTACTS][NI_KEY_LEN];
volatile uint32_t nb_tiebreak_calls;

static NiName nb_probe;
static uint32_t nb_state = NB_SEED;

const char *nb_dist_name(NbDist dist)
{
    static const char *const names[NB_DIST_COUNT] = {"random", "ascending", "descending", "prefix"};
    return names[dist];
}

/**
 * @brief Seed the xorshift32 generator (0 is mapped to NB_SEED).
 */
void nb_seed(uint32_t seed)
{
    nb_state = (seed != 0u) ? seed : NB_SEED;
}

uint32_t nb_rand(void)
{
    uint32_t x = nb_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    nb_state = x;
    return x;
}

uint32_t nb_rand_below(uint32_t n)
{
    return (uint32_t)(((uint64_t)nb_rand() * n) >> 32);
}

static uint16_t rand_range(uint16_t lo, uint16_t hi)
{
    return (uint16_t)(lo + nb_rand_below((uint32_t)(hi - lo)));
}

/**
 * @brief Draw one name from a distribution (never a reserved word).
 */
void nb_draw(NbDist dist, NiName *out)
{
    if (dist == NB_DIST_PREFIX)
    {
        bool shared = nb_rand_below(100) < 30u;
        out->first = shared ? rand_range(NB_FIRST_LONG_LO, NB_FIRST_COUNT) : rand_range(NB_FIRST_SHORT_LO, NB_FIRST_SHORT_HI);
    }
    else
    {
        out->first = rand_range(NB_FIRST_SHORT_LO, NB_FIRST_COUNT);
    }
    out->last = rand_range(NB_LAST_LO, NB_LAST_COUNT);
}

/**
 * @brief Draw a name that no stored contact has (surname "Qzx").
 */
void nb_draw_miss(NiName *out)
{
    out->first = rand_range(NB_FIRST_SHORT_LO, NB_FIRST_COUNT);
    out->last = NB_LAST_MISS;
}

/**
 * @brief Compose "First Last" into buf (at least NB_NAME_MAX bytes).
 */
void nb_compose(NiName name, char *buf)
{
    const char *f = nb_first_words[name.first];
    const char *l = nb_last_words[name.last];
    size_t i = 0;
    while (*f != '\0' && i < NB_NAME_MAX - 2) { buf[i++] = *f++; }
    buf[i++] = ' ';
    while (*l != '\0' && i < NB_NAME_MAX - 1) { buf[i++] = *l++; }
    buf[i] = '\0';
}

/**
 * @brief Assign a name to a sector and precompute its key.
 */
void nb_set(uint16_t sector, NiName name)
{
    char buf[NB_NAME_MAX];
    nb_names[sector] = name;
    nb_compose(name, buf);
    ni_make_key(buf, nb_keys[sector]);
}

/**
 * @brief Set the name that NI_PROBE_SECTOR resolves to in the tie-break.
 */
void nb_set_probe(NiName name)
{
    nb_probe = name;
}

/**
 * @brief Name sectors 0..n-1 from a distribution with the fixed seed.
 */
void nb_generate(NbDist dist, uint16_t n)
{
    nb_seed(NB_SEED ^ ((uint32_t)dist << 24));
    for (uint16_t s = 0; s < n; s++)
    {
        NiName name;
        nb_draw(dist, &name);
        nb_set(s, name);
    }
}

// Walks "First Last" one character at a time without composing it
typedef struct {
    const char *p;
    const char *last;
} NameCursor;

static inline int cursor_next(NameCursor *c)
{
    if (*c->p != '\0') { return (uint8_t)*c->p++; }
    if (c->last != NULL)
    {
        c->p = c->last;
        c->last = NULL;
        return ' ';
    }
    return 0;
}

static inline int fold(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

static int full_name_cmp(NiName a, NiName b, bool folded)
{
    NameCursor ca = {nb_first_words[a.first], nb_last_words[a.last]};
    NameCursor cb = {nb_first_words[b.first], nb_last_words[b.last]};
    for (;;)
    {
        int x = cursor_next(&ca);
        int y = cursor_next(&cb);
        if (folded) { x = fold(x); y = fold(y); }
        if (x != y) { return x - y; }
        if (x == 0) { return 0; }
    }
}

/**
 * @brief Tie-break: case-folded full names, then raw bytes, then sectors.
 *        NI_PROBE_SECTOR resolves to the probe name and ties at the name.
 */
int nb_tiebreak(uint16_t sector_a, uint16_t sector_b)
{
    nb_tiebreak_calls++;
    NiName a = (sector_a == NI_PROBE_SECTOR) ? nb_probe : nb_names[sector_a];
    NiName b = (sector_b == NI_PROBE_SECTOR) ? nb_probe : nb_names[sector_b];
    int c = full_name_cmp(a, b, true);
    if (c == 0) { c = full_name_cmp(a, b, false); }
    if (c != 0) { return c; }
    if (sector_a == NI_PROBE_SECTOR || sector_b == NI_PROBE_SECTOR) { return 0; }
    return (sector_a > sector_b) - (sector_a < sector_b);
}
