#ifndef EXAMPLE_H
#define EXAMPLE_H

#ifdef __cplusplus
extern "C" {
#endif

#define EXAMPLE_AIR780EP_BASIC_CONNECT  1
#define EXAMPLE_AIR780EP_MQTT_CLIENT    2
#define EXAMPLE_ML307R_PROBE            3

void example_air780ep_basic_connect_run(void);
void example_air780ep_mqtt_client_run(void);
void example_ml307r_probe_run(void);

#ifdef __cplusplus
}
#endif

#endif /* EXAMPLE_H */
