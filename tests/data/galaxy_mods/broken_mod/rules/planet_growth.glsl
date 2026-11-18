// Broken on purpose: each planet reads its neighbour's population, which the neighbour's
// own thread may already have changed in this stage.
void main() {
    FOR_EACH_PLANET(p) {
        int neighbour = planet_pop[(p + 1u) % terni.planet_count];
        planet_pop[p] += fx_mul(neighbour, D.growth_rate);
        planet_tax[p] = fx_mul(planet_pop[p], D.tax_rate);
    }
}
