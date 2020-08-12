extern "C" {
int _fltused = 0;
}

struct S {
    int a[4];
    char const *b;
    double c;
    float d;
};

long long hash(long long v)
{
    return v * 0x5851F42D4C957F2D + 0x14057B7EF767814F;
}

int test(S *s)
{
    int result = 0;
    for (int i = 0; i < 4; i++) {
        result ^= hash(s->a[i]);
        result ^= hash(s->b[i]);
    }
    result %= static_cast<int>(1.0f / (s->c * s->d));
    return result;
}

int main()
{
    S s;
    char h[5];
    for (int i = 0; i < 4; i++) {
        s.a[i] = hash(i);
        h[i] = 'A' + s.a[i] % ('Z' - 'A');
    }
    h[4] = '\0';
    s.b = h;
    s.c = 0.1;
    s.d = 0.1;
    return test(&s);
}
