// SPDX-License-Identifier: Apache-2.0

// 1D relativistic particle pusher in SI units: electrons accelerated by a
// uniform, constant electric field on a periodic domain, with charge
// deposition and energy diagnostics.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace constants {
const double c = 299792458.0;          // speed of light [m/s]
const double q_e = 1.602176634e-19;    // elementary charge [C]
const double m_e = 9.1093837015e-31;   // electron mass [kg]
const double eps0 = 8.8541878128e-12;  // vacuum permittivity [F/m]
}  // namespace constants

struct Particles {
  std::vector<double> x;        // position [m]
  std::vector<double> ux;       // gamma * v_x [m/s]
  std::vector<double> w;        // real particles per macro-particle
  std::vector<std::int64_t> id;
};

double lorentz_factor(double ux) {
  return sqrt(1.0 + ux * ux / (constants::c * constants::c));
}

// Total kinetic energy of all particles [J]
double kinetic_energy(const Particles& p) {
  double sum = 0.0;
  for (std::size_t i = 0; i < p.x.size(); ++i) {
    const double gamma = lorentz_factor(p.ux[i]);
    sum += p.w[i] * constants::m_e * constants::c * constants::c * (gamma - 1.0);
  }
  return sum;
}

// Advance momenta and positions by one step in the field E [V/m]
void push(Particles& p, double E, double dt, double length) {
  const double qm = -constants::q_e / constants::m_e;
  for (std::size_t i = 0; i < p.x.size(); ++i) {
    p.ux[i] += qm * E * dt;
    const double gamma = lorentz_factor(p.ux[i]);
    p.x[i] += p.ux[i] / gamma * dt;
    if (p.x[i] < 0.0) p.x[i] += length;
    if (p.x[i] >= length) p.x[i] -= length;
  }
}

// Nearest-grid-point charge density [C/m^3]
void deposit(const Particles& p, std::vector<double>& rho, double dx) {
  std::fill(rho.begin(), rho.end(), 0.0);
  for (std::size_t i = 0; i < p.x.size(); ++i) {
    const int cell = (int)floor(p.x[i] / dx);
    rho[cell] += -constants::q_e * p.w[i] / dx;
  }
}

// Electron plasma frequency [rad/s] for a number density n [1/m^3]
double plasma_frequency(double n) {
  return sqrt(std::max(n, 1e-300) * constants::q_e * constants::q_e /
              (constants::m_e * constants::eps0));
}

int main() {
  const int nx = 4096;
  const std::int64_t particles_per_cell = 16;
  const std::int64_t np = nx * particles_per_cell;
  const double length = 1.0e-2;   // domain size [m]
  const double dx = length / nx;
  const double density = 1.0e24;  // [1/m^3]
  const double E = -1.0e3;        // [V/m]
  const double dt = 0.5 * dx / constants::c;
  const double t_end = 2000 * dt;

  Particles p;
  for (std::int64_t i = 0; i < np; ++i) {
    p.x.push_back((i + 0.5) / np * length);
    p.ux.push_back(1.0e5 * sin(0.37 * i));  // non-relativistic drift spread
    p.w.push_back(density * length / np);
    p.id.push_back(i);
  }

  const double ke_start = kinetic_energy(p);
  double t = 0.0;
  std::int64_t step = 0;
  while (t < t_end) {
    push(p, E, dt, length);
    t += dt;
    ++step;
  }
  const double ke_end = kinetic_energy(p);

  std::vector<double> rho(nx);
  deposit(p, rho, dx);
  double charge = 0.0;
  double mean_x = 0.0;
  for (double r : rho) charge += r * dx;
  for (double x : p.x) mean_x += x;
  mean_x /= np;

  printf("steps:                %lld\n", (long long)step);
  printf("time [s]:             %.12e\n", t);
  printf("kinetic energy start: %.12e\n", ke_start);
  printf("kinetic energy end:   %.12e\n", ke_end);
  printf("energy gain [J]:      %.12e\n", ke_end - ke_start);
  printf("mean position [m]:    %.12e\n", mean_x);
  printf("total charge [C]:     %.12e\n", charge);
  printf("plasma frequency:     %.12e\n", plasma_frequency(density));

  const double expected_charge = -constants::q_e * density * length;
  if (fabs(charge - expected_charge) > 1e-12 * fabs(expected_charge)) {
    printf("FAILED: charge is not conserved\n");
    return 1;
  }
  printf("PASSED\n");
  return 0;
}
