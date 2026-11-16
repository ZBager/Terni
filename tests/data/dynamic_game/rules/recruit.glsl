void main() {
    FOR_EACH_PROVINCE(p) {
        if ((terni.tick + p) % 3u == 0u) province_raise[p] += 1u + p;
    }
}
