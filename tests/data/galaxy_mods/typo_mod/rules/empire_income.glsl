// Does not compile: a missing semicolon.
void main() {
    FOR_EACH_PLANET(p) {
        atomicAdd(empire_income[planet_owner[p]], to_agg(planet_tax[p]))
    }
}
