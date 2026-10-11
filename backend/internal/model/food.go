package model

type FoodItem struct {
	ID   int    `json:"id" gorm:"primaryKey;autoIncrement"`
	Name string `json:"name" gorm:"type:varchar(100);unique;not null"`
}

func (FoodItem) TableName() string { return "food_items" }
