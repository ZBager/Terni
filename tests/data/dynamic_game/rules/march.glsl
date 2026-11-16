void main() {
    FOR_EACH_ARMY(a) {
        army_strength[a] -= 1 + int(army_home[a]);
        if (army_strength[a] <= 0) army_destroy(a);
    }
}
