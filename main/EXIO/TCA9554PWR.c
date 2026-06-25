#include "TCA9554PWR.h"
/*****************************************************  Operation register REG   ****************************************************/   
uint8_t Read_REG(uint8_t REG)
{
    uint8_t bitsStatus = 0;
    I2C_Read(TCA9554_ADDRESS, REG, &bitsStatus, 1);
    return bitsStatus;
}
void Write_REG(uint8_t REG, uint8_t Data)
{
    I2C_Write(TCA9554_ADDRESS, REG, &Data, 1);
}
/********************************************************** Set EXIO mode **********************************************************/       
void Mode_EXIO(uint8_t Pin,uint8_t State)                 // Set the mode of the TCA9554PWR Pin. The default is Output mode (output mode or input mode). State: 0= Output mode 1= input mode    
{
    uint8_t bitsStatus = Read_REG(TCA9554_CONFIG_REG);                                
    uint8_t Data = (0x01 << (Pin-1)) | bitsStatus;  
    Write_REG(TCA9554_CONFIG_REG,Data);            
}
void Mode_EXIOS(uint8_t PinState)                        // Set the mode of the 7 pins from the TCA9554PWR with PinState   
{
    Write_REG(TCA9554_CONFIG_REG,PinState);                             
}

/********************************************************** Read EXIO status **********************************************************/       
uint8_t Read_EXIO(uint8_t Pin)                            // Read the level of the TCA9554PWR Pin
{
    uint8_t inputBits =Read_REG(TCA9554_INPUT_REG);                                  
    uint8_t bitStatus = (inputBits >> (Pin-1)) & 0x01;                             
    return bitStatus;                                                              
}
uint8_t Read_EXIOS(void)                                  // Read the level of all pins of TCA9554PWR
{
  uint8_t inputBits = Read_REG(TCA9554_INPUT_REG);                                     
  return inputBits;                                                                    
}

/********************************************************** Set the EXIO output status **********************************************************/  
void Set_EXIO(uint8_t Pin,uint8_t State)                  // Sets the level state of the Pin without affecting the other pins(PIN：1~8)
{
    uint8_t Data = 0;
    uint8_t bitsStatus = Read_REG(TCA9554_OUTPUT_REG);         
    if(State < 2 && Pin < 9 && Pin > 0){     
        if(State == 1)                                     
            Data = (0x01 << (Pin-1)) | bitsStatus;                 
        else if(State == 0) 
            Data = (~(0x01 << (Pin-1)) & bitsStatus);  
        Write_REG(TCA9554_OUTPUT_REG,Data);
    }
    else                                                                             
        printf("Parameter error, please enter the correct parameter!\r\n");

}
void Set_EXIOS(uint8_t PinState)                     // Set 7 pins to the PinState state such as :PinState=0x23, 0010 0011 state (the highest bit is not used)
{
    Write_REG(TCA9554_OUTPUT_REG,PinState);                                            
}

/********************************************************** Flip EXIO state **********************************************************/  
void Set_Toggle(uint8_t Pin)                              // Flip the level of the TCA9554PWR Pin
{
    uint8_t bitsStatus = Read_EXIO(Pin);                                              
    Set_EXIO(Pin,(bool)!bitsStatus);
}


/********************************************************* TCA9554PWR Initializes the device ***********************************************************/  
void TCA9554PWR_Init(uint8_t PinState)                  // Set the seven pins to PinState state, for example :PinState=0x23, 0010 0011 State (the highest bit is not used) (Output mode or input mode) 0= Output mode 1= Input mode. The default value is output mode
{
    // i2c_master_init();                                                  
    Mode_EXIOS(PinState);                                          
}

esp_err_t EXIO_Init(void)
{
    TCA9554PWR_Init(0x00);
    // Force all EXIO outputs LOW at boot. The TCA9554 OUTPUT register powers
    // up HIGH, which would otherwise drive the on-board buzzer pin and any
    // active-high peripherals until something else writes them.
    Set_EXIOS(0x00);
    return ESP_OK;
}
