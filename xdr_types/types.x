enum tagtype {
    GET = 0,
    OK = 1,
    QUIT = 2,
    ERR = 3
};

struct file {
    opaque contents<>;
    unsigned int last_mod_time;
};

union message switch (tagtype tag) {
    case GET:
        string filename<256>;
    case OK:
        struct file fdata;
    case QUIT:
        void;
    case ERR:
        void;
};