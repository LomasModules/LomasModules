
import numpy
import matplotlib.pyplot as plt

WAVETABLE_SIZE = 1024

waveforms = []

"""----------------------------------------------------------------------------
Sine wave
----------------------------------------------------------------------------"""


env_linear = numpy.arange(0, 256.0) / 255.0

#env_linear[-1] = env_linear[-2]
#env_quartic = env_linear ** 3
env_expo = (1.0 - numpy.exp(-3 * env_linear)) * 1.052395620771343
env_quartic = env_linear ** 3


fig, ax = plt.subplots()
ax.plot(env_expo)
ax.plot(env_linear)
ax.plot(env_quartic)

ax.set(xlabel='time (s)', ylabel='voltage (mV)',
       title='About as simple as it gets, folks')
ax.grid()

plt.show()

numpy.savetxt("ENV_EXPO", env_expo, fmt = '%.6f',  newline ='f,',)
numpy.savetxt("ENV_QUARTIC", env_quartic, fmt = '%.6f',  newline ='f,',)

#import matplotlib.pyplot as plt
#import numpy as np

#t = np.linspace(-5, 1, 256)
#s = np.exp(t)

#a = t * s

#fig, ax = plt.subplots()
#ax.plot(t, s)

#ax.set(xlabel='time (s)', ylabel='voltage (mV)',
#       title='About as simple as it gets, folks')
#ax.grid()

#plt.show()

#np.savetxt("lutnpy", a)