#ifndef EXAMPLE_H
#define EXAMPLE_H

#ifdef __cplusplus
extern "C" {
#endif

#define EXAMPLE_BASIC_CONNECT  1
#define EXAMPLE_MQTT_CLIENT    2
#define EXAMPLE_ML307R_PROBE   3

void example_basic_connect_run(void);
void example_mqtt_client_run(void);
void example_ml307r_probe_run(void);

#ifdef __cplusplus
}
#endif

#endif /* EXAMPLE_H */
