void main() {
    FOR_EACH_TOWN(t) {
        town_level_set(t, town_level(t) + 1u);
    }
}
